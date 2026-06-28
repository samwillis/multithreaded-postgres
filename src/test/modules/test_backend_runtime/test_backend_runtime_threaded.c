/*--------------------------------------------------------------------------
 *
 * test_backend_runtime_threaded.c
 *		Thread-safe helper module for backend runtime TAP tests
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_backend_runtime/test_backend_runtime_threaded.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/buf_internals.h"
#include "storage/condition_variable.h"
#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/lwlock.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/backend_runtime.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_backend_runtime_threaded",
					.version = PG_VERSION,
					PG_MODULE_MAGIC_BACKEND_MODEL_POOLED_PROTOCOL_AFFINE
);

PG_FUNCTION_INFO_V1(test_backend_runtime_model_snapshot);
PG_FUNCTION_INFO_V1(test_backend_runtime_request_autovacuum_worker);
PG_FUNCTION_INFO_V1(test_backend_runtime_rejects_process_bgworker);
PG_FUNCTION_INFO_V1(test_backend_runtime_launch_thread_bgworker);
PG_FUNCTION_INFO_V1(test_backend_runtime_restart_thread_bgworker);
PG_FUNCTION_INFO_V1(test_backend_runtime_crash_thread_bgworker);
PG_FUNCTION_INFO_V1(test_backend_runtime_custom_guc_value);
PG_FUNCTION_INFO_V1(test_backend_runtime_custom_guc_init_count);
PG_FUNCTION_INFO_V1(test_backend_runtime_guc_reset_baseline_matches);
PG_FUNCTION_INFO_V1(test_backend_runtime_buffer_refcount_state_is_reusable);
PG_FUNCTION_INFO_V1(test_backend_runtime_emit_fatal);
PG_FUNCTION_INFO_V1(test_backend_runtime_wait_completion_enabled);
PG_FUNCTION_INFO_V1(test_backend_runtime_wait_completion_snapshot);
PG_FUNCTION_INFO_V1(test_backend_runtime_protocol_park_snapshot);
PG_FUNCTION_INFO_V1(test_backend_runtime_wait_on_condition_variable);
PG_FUNCTION_INFO_V1(test_backend_runtime_hold_lwlock);
PG_FUNCTION_INFO_V1(test_backend_runtime_wait_on_lwlock);
PG_FUNCTION_INFO_V1(test_backend_runtime_reusable_session_validator_reasons);

pg_noreturn PGDLLEXPORT void test_backend_runtime_unreachable_bgworker_main(Datum main_arg);
PGDLLEXPORT void test_backend_runtime_thread_bgworker_main(Datum main_arg);
PGDLLEXPORT void test_backend_runtime_restart_bgworker_main(Datum main_arg);
pg_noreturn PGDLLEXPORT void test_backend_runtime_crash_bgworker_main(Datum main_arg);
PGDLLEXPORT void _PG_init(void);

static uint32 test_backend_runtime_thread_bgworker_wait_event = 0;
static uint32 test_backend_runtime_condition_variable_wait_event = 0;
static uint32 test_backend_runtime_hold_lwlock_wait_event = 0;
static bool test_backend_runtime_lwlock_initialized = false;
static LWLock test_backend_runtime_lwlock;
static pg_atomic_uint32 test_backend_runtime_restart_count;
static pg_atomic_uint32 test_backend_runtime_crash_count;
static PG_THREAD_LOCAL char *test_backend_runtime_custom_guc = NULL;
static PG_THREAD_LOCAL int test_backend_runtime_custom_guc_init_counter = 0;

static void
test_backend_runtime_validator_init_clean(PgBackend *backend,
										  PgSession *session,
										  PgConnection *connection,
										  PgExecution *execution)
{
	MemSet(backend, 0, sizeof(*backend));
	MemSet(session, 0, sizeof(*session));
	MemSet(connection, 0, sizeof(*connection));
	MemSet(execution, 0, sizeof(*execution));

	PgBackendInitializeInterrupts(backend);
	backend->session = session;
	backend->connection = connection;
	backend->execution = execution;
	backend->my_proc_number = INVALID_PROC_NUMBER;
	backend->parallel_leader_proc_number = INVALID_PROC_NUMBER;
	dlist_init(&backend->dsm_segment_list);
	dlist_init(&backend->pgstat_pending.pending);
	backend->buffers.reserved_ref_count_slot = -1;
	backend->buffers.private_ref_count_entry_last = -1;
	dlist_init(&backend->storage.smgr_unpinned_relations);

	session->backend = backend;
	session->connection = connection;
	session->execution = execution;
	session->guc.initialized = true;
	dlist_init(&session->guc.nondef_list);
	slist_init(&session->guc.stack_list);
	slist_init(&session->guc.report_list);
	session->temp_file.initialized = true;
	session->temp_file.num_temp_table_spaces = -1;
	session->plan_cache.initialized = true;
	dlist_init(&session->plan_cache.saved_plan_list);
	dlist_init(&session->plan_cache.cached_expression_list);
	session->namespace_state.my_temp_namespace = InvalidOid;
	session->namespace_state.my_temp_toast_namespace = InvalidOid;
	session->namespace_state.my_temp_namespace_subid = InvalidSubTransactionId;

	connection->backend = backend;
	connection->session = session;

	execution->backend = backend;
	execution->session = session;
}

static HTAB *
test_backend_runtime_validator_create_locallock_hash(bool with_entry)
{
	HASHCTL		info;
	HTAB	   *hash;

	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(LOCALLOCKTAG);
	info.entrysize = sizeof(LOCALLOCK);

	hash = hash_create("test backend runtime LOCALLOCK hash",
					   16,
					   &info,
					   HASH_ELEM | HASH_BLOBS);
	if (with_entry)
	{
		LOCALLOCKTAG localtag;
		bool		found;

		MemSet(&localtag, 0, sizeof(localtag));
		localtag.lock.locktag_lockmethodid = DEFAULT_LOCKMETHOD;
		localtag.mode = AccessShareLock;

		(void) hash_search(hash, &localtag, HASH_ENTER, &found);
	}

	return hash;
}

static void
test_backend_runtime_validator_expect(PgReusableSessionValidationReason expected,
									  PgBackend *backend,
									  PgSession *session,
									  PgConnection *connection,
									  PgExecution *execution,
									  const char *label)
{
	PgReusableSessionValidationReason reason;

	reason = PgValidateReusableSessionState(backend, session, connection,
											execution, false);
	if (reason != expected)
		elog(ERROR,
			 "reusable session validator returned %s for %s, expected %s",
			 PgReusableSessionValidationReasonName(reason),
			 label,
			 PgReusableSessionValidationReasonName(expected));
}

static const char *
test_backend_runtime_kind_name(PgRuntimeKind kind)
{
	switch (kind)
	{
		case PG_RUNTIME_PROCESS:
			return "process";
		case PG_RUNTIME_THREAD_PER_SESSION:
			return "thread_per_session";
		case PG_RUNTIME_POOLED_PROTOCOL:
			return "pooled_protocol";
	}

	return "unknown";
}

Datum
test_backend_runtime_reusable_session_validator_reasons(PG_FUNCTION_ARGS)
{
	PgBackend	backend;
	PgSession	session;
	PgConnection connection;
	PgExecution execution;
	Buffer		private_ref_count_array_keys[REFCOUNT_ARRAY_ENTRIES];
	PrivateRefCountEntry private_ref_count_array[REFCOUNT_ARRAY_ENTRIES];
	int			fast_path_local_use_counts[FP_LOCK_GROUPS_PER_BACKEND_MAX];
	dlist_node	dsm_node;
	dlist_node	storage_node;
	dlist_node	plan_cache_node;
	HTAB	   *lock_hash;

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_VALID,
										  &backend, &session, &connection,
										  &execution, "clean runtime state");

	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_NULL_OBJECT,
										  NULL, &session, &connection,
										  &execution, "missing backend");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.my_proc = (struct PGPROC *) &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROC_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "attached PGPROC");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.my_proc_number = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROC_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "attached proc number");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.my_beentry = (PgBackendStatus *) &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PGSTAT_STATE,
										  &backend, &session, &connection,
										  &execution, "attached pgstat entry");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.pgstat_pending.local = (PgStat_LocalState *) &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PGSTAT_STATE,
										  &backend, &session, &connection,
										  &execution, "pending pgstat local state");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.ipc.proc_signal_slot = &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_IPC_STATE,
										  &backend, &session, &connection,
										  &execution, "attached proc signal slot");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.ipc.shared_invalidation_messages = &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_IPC_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "shared invalidation messages");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	pg_atomic_write_u32(&backend.interrupts.pending_mask,
						PG_BACKEND_INTERRUPT_MASK(PG_BACKEND_INTERRUPT_PROC_SIGNAL_BARRIER));
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_IPC_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "pending backend interrupt");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.interrupts.proc_die_sender_pid = 123;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_IPC_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "proc die sender identity");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.ipc.dsm_registry_table = &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_DSM_SEGMENTS,
										  &backend, &session, &connection,
										  &execution, "dsm registry table");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.ipc.next_local_transaction_id = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "next local transaction id");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.transaction.procarray_cached_xid_not_in_progress =
		FirstNormalTransactionId;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "cached completed transaction id");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.transaction.compute_xid_horizons_result_last_xmin =
		FirstNormalTransactionId;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "cached procarray xmin horizon");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.transaction.global_vis_data_rels.maybe_needed =
		FirstNormalFullTransactionId;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE,
										  &backend, &session, &connection,
										  &execution,
										  "global visibility horizon");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.startup.client_socket = (struct ClientSocket *) &connection;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "attached client socket");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.identity.port = (struct Port *) &connection;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "connection identity port");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.identity.cancel_key[0] = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "connection cancel key");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.socket_io.send_buffer = (char *) &connection;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "socket send buffer");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.protocol.fe_be_wait_set = (WaitEventSet *) &connection;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "frontend/backend wait set");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.client_connection_info.authn_id = "authn";
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "client auth identity");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	connection.security.gss_send_buffer = (char *) &connection;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED,
										  &backend, &session, &connection,
										  &execution, "GSS send buffer");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.prepared_statement.prepared_queries = (HTAB *) &session;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PREPARED_STATEMENTS,
										  &backend, &session, &connection,
										  &execution, "prepared statement cache");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.portal_manager.portal_hash_table = (HTAB *) &session;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PORTALS,
										  &backend, &session, &connection,
										  &execution, "portal hash");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.async.registered_listener = true;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_LISTEN,
										  &backend, &session, &connection,
										  &execution, "registered listener");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.namespace_state.my_temp_namespace = (Oid) 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_TEMP_NAMESPACE,
										  &backend, &session, &connection,
										  &execution, "temp namespace");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.extension_modules.private_states = (List *) &session;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_EXTENSION_STATE,
										  &backend, &session, &connection,
										  &execution, "session extension private state");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	dlist_push_head(&backend.dsm_segment_list, &dsm_node);
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_DSM_SEGMENTS,
										  &backend, &session, &connection,
										  &execution, "dsm segment list");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.resource_owners.current_owner =
		(struct ResourceOwnerData *) &execution;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_RESOURCE_OWNER,
										  &backend, &session, &connection,
										  &execution, "resource owner");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.memory_contexts.top_context = (MemoryContext) &execution;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_MEMORY_CONTEXTS,
										  &backend, &session, &connection,
										  &execution, "memory context");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.timeout.num_active_timeouts = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_ACTIVE_TIMEOUTS,
										  &backend, &session, &connection,
										  &execution, "active timeout");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.locks.num_held_lwlocks = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_LOCKS,
										  &backend, &session, &connection,
										  &execution, "held lwlock");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	lock_hash = test_backend_runtime_validator_create_locallock_hash(false);
	backend.locks.lock_method_local_hash = lock_hash;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_VALID,
										  &backend, &session, &connection,
										  &execution, "empty local lock hash");
	hash_destroy(lock_hash);

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	lock_hash = test_backend_runtime_validator_create_locallock_hash(true);
	backend.locks.lock_method_local_hash = lock_hash;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_LOCKS,
										  &backend, &session, &connection,
										  &execution, "local lock hash entry");
	hash_destroy(lock_hash);

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	MemSet(fast_path_local_use_counts, 0,
		   sizeof(fast_path_local_use_counts));
	fast_path_local_use_counts[0] = 1;
	backend.locks.fast_path_local_use_counts =
		fast_path_local_use_counts;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_LOCKS,
										  &backend, &session, &connection,
										  &execution, "fast path lock count");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.buffers.n_local_pinned_buffers = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_BUFFER_PINS,
										  &backend, &session, &connection,
										  &execution, "local buffer pin");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	MemSet(private_ref_count_array_keys, 0,
		   sizeof(private_ref_count_array_keys));
	MemSet(private_ref_count_array, 0,
		   sizeof(private_ref_count_array));
	backend.buffers.private_ref_count_array_keys =
		private_ref_count_array_keys;
	backend.buffers.private_ref_count_array =
		private_ref_count_array;
	private_ref_count_array_keys[0] = (Buffer) 1;
	private_ref_count_array[0].buffer = (Buffer) 1;
	private_ref_count_array[0].data.refcount = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_BUFFER_PINS,
										  &backend, &session, &connection,
										  &execution,
										  "resident private buffer refcount");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.storage.num_external_fds = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_STORAGE_STATE,
										  &backend, &session, &connection,
										  &execution, "external file descriptor");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.storage.nfile = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_STORAGE_STATE,
										  &backend, &session, &connection,
										  &execution, "open virtual file");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.storage.num_allocated_descs = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_STORAGE_STATE,
										  &backend, &session, &connection,
										  &execution, "allocated descriptor");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	backend.storage.sync_pending_ops = (HTAB *) &backend;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_STORAGE_STATE,
										  &backend, &session, &connection,
										  &execution, "pending sync ops");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	dlist_push_head(&backend.storage.smgr_unpinned_relations, &storage_node);
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_STORAGE_STATE,
										  &backend, &session, &connection,
										  &execution, "unpinned smgr relation list");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.temp_file.temporary_files_size = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_TEMP_FILES,
										  &backend, &session, &connection,
										  &execution, "temporary file bytes");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.guc.nest_level = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_GUC_STATE,
										  &backend, &session, &connection,
										  &execution, "guc nesting");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	dlist_push_head(&session.plan_cache.saved_plan_list, &plan_cache_node);
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_PLAN_CACHE,
										  &backend, &session, &connection,
										  &execution, "saved plan cache list");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	session.invalidation_callbacks.relcache_callback_count = 1;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_INVALIDATIONS,
										  &backend, &session, &connection,
										  &execution, "relcache callback");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.snapshot.first_snapshot_set = true;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_SNAPSHOTS,
										  &backend, &session, &connection,
										  &execution, "first snapshot");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.invalidation.trans_info =
		(struct TransInvalidationInfo *) &execution;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_INVALIDATIONS,
										  &backend, &session, &connection,
										  &execution, "transaction invalidation state");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.xloginsert.begininsert_called = true;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_XLOG_INSERT_STATE,
										  &backend, &session, &connection,
										  &execution, "xlog insert in progress");

	test_backend_runtime_validator_init_clean(&backend, &session,
											  &connection, &execution);
	execution.async.pending_actions = (struct ActionList *) &execution;
	test_backend_runtime_validator_expect(PG_REUSABLE_SESSION_INVALID_ASYNC_ACTIONS,
										  &backend, &session, &connection,
										  &execution, "pending async action");

	PG_RETURN_BOOL(true);
}

static const char *
test_backend_runtime_model_name(PgBackendModel model)
{
	switch (model)
	{
		case PG_BACKEND_MODEL_PROCESS:
			return "process";
		case PG_BACKEND_MODEL_THREAD_PER_SESSION:
			return "thread-per-session";
		case PG_BACKEND_MODEL_POOLED_SCHEDULER:
			return "pooled-scheduler";
		case PG_BACKEND_MODEL_POOLED_PROTOCOL_AFFINE:
			return "pooled-protocol-affine";
		case PG_BACKEND_MODEL_POOLED_PROTOCOL_MIGRATABLE:
			return "pooled-protocol-migratable";
		case PG_BACKEND_MODEL_TASK_REENTRANT:
			return "task-reentrant";
	}

	return "unknown";
}

static const char *
test_backend_runtime_wait_kind_name(PgWaitKind kind)
{
	switch (kind)
	{
		case PG_WAIT_KIND_NONE:
			return "none";
		case PG_WAIT_KIND_EVENT_SET:
			return "event_set";
		case PG_WAIT_KIND_SEMAPHORE:
			return "semaphore";
	}

	return "unknown";
}

static const char *
test_backend_runtime_wait_completion_state_name(uint32 state)
{
	switch (state)
	{
		case PG_WAIT_COMPLETION_INACTIVE:
			return "inactive";
		case PG_WAIT_COMPLETION_WAITING:
			return "waiting";
		case PG_WAIT_COMPLETION_READY:
			return "ready";
		case PG_WAIT_COMPLETION_CANCELLED:
			return "cancelled";
	}

	return "unknown";
}

static const char *
test_backend_runtime_protocol_park_state_name(PgProtocolParkState state)
{
	switch (state)
	{
		case PG_PROTOCOL_PARK_NONE:
			return "none";
		case PG_PROTOCOL_PARK_PREPARED:
			return "prepared";
		case PG_PROTOCOL_PARK_COMMITTED:
			return "committed";
	}

	return "unknown";
}

static const char *
test_backend_runtime_protocol_queue_state_name(
	PgProtocolSchedulerQueueState state)
{
	switch (state)
	{
		case PG_PROTOCOL_SCHEDULER_QUEUE_NONE:
			return "none";
		case PG_PROTOCOL_SCHEDULER_QUEUE_PARKED_PROTOCOL_READ:
			return "parked_protocol_read";
		case PG_PROTOCOL_SCHEDULER_QUEUE_POLLING:
			return "polling";
		case PG_PROTOCOL_SCHEDULER_QUEUE_RUNNABLE:
			return "runnable";
		case PG_PROTOCOL_SCHEDULER_QUEUE_LEASED:
			return "leased";
	}

	return "unknown";
}

void
_PG_init(void)
{
	test_backend_runtime_custom_guc_init_counter++;

	DefineCustomStringVariable("test_backend_runtime_threaded.custom_guc",
							   "Test threaded custom GUC.",
							   NULL,
							   &test_backend_runtime_custom_guc,
							   "default",
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	if (!test_backend_runtime_lwlock_initialized)
	{
		int			tranche_id;

		tranche_id = LWLockNewTrancheId("TestBackendRuntimeLWLock");
		LWLockInitialize(&test_backend_runtime_lwlock, tranche_id);
		test_backend_runtime_lwlock_initialized = true;
	}
}

Datum
test_backend_runtime_model_snapshot(PG_FUNCTION_ARGS)
{
	PgRuntime  *runtime = CurrentPgRuntime;
	char	   *result;

	if (runtime == NULL)
		PG_RETURN_NULL();

	result = psprintf("%s|%s|%d|%d",
					  test_backend_runtime_kind_name(runtime->kind),
					  test_backend_runtime_model_name(
						  runtime->extension_backend_model),
					  PgRuntimePooledProtocolCarrierLimit(),
					  PgRuntimePooledProtocolRequested());

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
test_backend_runtime_request_autovacuum_worker(PG_FUNCTION_ARGS)
{
	SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER);

	PG_RETURN_BOOL(true);
}

Datum
test_backend_runtime_rejects_process_bgworker(PG_FUNCTION_ARGS)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, MAXPGPATH, "test_backend_runtime_threaded");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "test_backend_runtime_unreachable_bgworker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "test_backend_runtime process bgworker");
	snprintf(worker.bgw_type, BGW_MAXLEN,
			 "test_backend_runtime process bgworker");
	worker.bgw_notify_pid = PgCurrentBackendSignalPid();

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		elog(ERROR, "could not register process-model background worker");

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STOPPED)
	{
		if (status == BGWH_STARTED)
			TerminateBackgroundWorker(handle);
		elog(ERROR, "process-model background worker was not rejected: status %d",
			 status);
	}

	PG_RETURN_BOOL(true);
}

Datum
test_backend_runtime_launch_thread_bgworker(PG_FUNCTION_ARGS)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status = BGWH_NOT_YET_STARTED;
	pid_t		pid = 0;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_backend_model = BgWorkerBackendThreadPerSession;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, MAXPGPATH, "test_backend_runtime_threaded");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "test_backend_runtime_thread_bgworker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "test_backend_runtime thread bgworker");
	snprintf(worker.bgw_type, BGW_MAXLEN,
			 "test_backend_runtime thread bgworker");
	worker.bgw_notify_pid = PgCurrentBackendSignalPid();

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		elog(ERROR, "could not register thread-model background worker");

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		elog(ERROR, "thread-model background worker did not start: status %d",
			 status);

	TerminateBackgroundWorker(handle);
	status = WaitForBackgroundWorkerShutdown(handle);
	if (status != BGWH_STOPPED)
		elog(ERROR, "thread-model background worker did not stop: status %d",
			 status);

	PG_RETURN_INT32(pid);
}

Datum
test_backend_runtime_restart_thread_bgworker(PG_FUNCTION_ARGS)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status = BGWH_NOT_YET_STARTED;
	pid_t		pid = 0;
	bool		restarted = false;

	pg_atomic_init_u32(&test_backend_runtime_restart_count, 0);

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_backend_model = BgWorkerBackendThreadPerSession;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 1;
	snprintf(worker.bgw_library_name, MAXPGPATH, "test_backend_runtime_threaded");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "test_backend_runtime_restart_bgworker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "test_backend_runtime restart bgworker");
	snprintf(worker.bgw_type, BGW_MAXLEN,
			 "test_backend_runtime restart bgworker");
	worker.bgw_notify_pid = PgCurrentBackendSignalPid();

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		elog(ERROR, "could not register restartable thread-model background worker");

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED &&
		!(status == BGWH_STOPPED &&
		  pg_atomic_read_u32(&test_backend_runtime_restart_count) >= 1))
		elog(ERROR, "restartable thread-model background worker did not start: status %d",
			 status);

	for (int i = 0; i < 50; i++)
	{
		PgCurrentBackendApplyInterrupts();
		CHECK_FOR_INTERRUPTS();

		if (pg_atomic_read_u32(&test_backend_runtime_restart_count) >= 2)
		{
			restarted = true;
			break;
		}

		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 100L,
						 WAIT_EVENT_BGWORKER_STARTUP);
		ResetLatch(MyLatch);
	}

	if (!restarted)
	{
		TerminateBackgroundWorker(handle);
		(void) WaitForBackgroundWorkerShutdown(handle);
		elog(ERROR, "restartable thread-model background worker did not restart");
	}

	TerminateBackgroundWorker(handle);
	status = WaitForBackgroundWorkerShutdown(handle);
	if (status != BGWH_STOPPED)
		elog(ERROR, "restartable thread-model background worker did not stop: status %d",
			 status);

	PG_RETURN_BOOL(true);
}

Datum
test_backend_runtime_crash_thread_bgworker(PG_FUNCTION_ARGS)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status = BGWH_NOT_YET_STARTED;
	pid_t		pid = 0;

	pg_atomic_init_u32(&test_backend_runtime_crash_count, 0);

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_backend_model = BgWorkerBackendThreadPerSession;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	snprintf(worker.bgw_library_name, MAXPGPATH, "test_backend_runtime_threaded");
	snprintf(worker.bgw_function_name, BGW_MAXLEN,
			 "test_backend_runtime_crash_bgworker_main");
	snprintf(worker.bgw_name, BGW_MAXLEN,
			 "test_backend_runtime crash bgworker");
	snprintf(worker.bgw_type, BGW_MAXLEN,
			 "test_backend_runtime crash bgworker");
	worker.bgw_notify_pid = PgCurrentBackendSignalPid();

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		elog(ERROR, "could not register crashing thread-model background worker");

	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		elog(ERROR, "crashing thread-model background worker did not start: status %d",
			 status);

	(void) WaitForBackgroundWorkerShutdown(handle);

	elog(ERROR, "crashing thread-model background worker did not terminate the threaded runtime");
	PG_RETURN_BOOL(false);
}

Datum
test_backend_runtime_custom_guc_value(PG_FUNCTION_ARGS)
{
	if (test_backend_runtime_custom_guc == NULL)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(test_backend_runtime_custom_guc));
}

Datum
test_backend_runtime_custom_guc_init_count(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(test_backend_runtime_custom_guc_init_counter);
}

Datum
test_backend_runtime_guc_reset_baseline_matches(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(GUCStateMatchesResetBaseline());
}

Datum
test_backend_runtime_buffer_refcount_state_is_reusable(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(BufferManagerPrivateRefCountStateIsReusable(
					   &CurrentPgBackend->buffers));
}

Datum
test_backend_runtime_emit_fatal(PG_FUNCTION_ARGS)
{
	ereport(FATAL,
			(errmsg("test_backend_runtime requested FATAL")));
	pg_unreachable();
}

Datum
test_backend_runtime_wait_completion_enabled(PG_FUNCTION_ARGS)
{
#ifdef PG_RUNTIME_ENABLE_WAIT_COMPLETION_PUBLICATION
	PG_RETURN_BOOL(true);
#else
	PG_RETURN_BOOL(false);
#endif
}

Datum
test_backend_runtime_wait_completion_snapshot(PG_FUNCTION_ARGS)
{
	int32		backend_id_arg = PG_GETARG_INT32(0);
	PgWaitCompletion snapshot;
	uint32		waiting;
	uint32		state;
	uint32		ready_events;
	uint32		interrupt_events;
	const char *wait_event;
	char	   *result;

	if (backend_id_arg <= 0)
		PG_RETURN_NULL();

	if (!PgBackendSnapshotWaitCompletionById((PgBackendId) backend_id_arg,
											 &snapshot, &waiting))
		PG_RETURN_NULL();

	state = pg_atomic_read_u32(&snapshot.state);
	ready_events = pg_atomic_read_u32(&snapshot.ready_events);
	interrupt_events = pg_atomic_read_u32(&snapshot.interrupt_events);
	wait_event = pgstat_get_wait_event(snapshot.spec.wait_event_info);
	if (wait_event == NULL)
		wait_event = "";

	result = psprintf("%s|%s|%s|%u|%u|%u|%u|%d|%d|%d",
					  test_backend_runtime_wait_completion_state_name(state),
					  test_backend_runtime_wait_kind_name(snapshot.spec.kind),
					  wait_event,
					  waiting,
					  snapshot.spec.wake_events,
					  ready_events,
					  interrupt_events,
					  snapshot.backend != NULL,
					  snapshot.session != NULL,
					  snapshot.execution != NULL);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
test_backend_runtime_protocol_park_snapshot(PG_FUNCTION_ARGS)
{
	int32		backend_id_arg = PG_GETARG_INT32(0);
	PgProtocolParkSnapshot snapshot;
	char	   *result;

	if (backend_id_arg <= 0)
		PG_RETURN_NULL();

	if (!PgBackendSnapshotProtocolParkById((PgBackendId) backend_id_arg,
										   &snapshot))
		PG_RETURN_NULL();

	result = psprintf("%s|%s|"
					  UINT64_FORMAT "|%u|%u|" UINT64_FORMAT "|%u|%u|"
					  UINT64_FORMAT "|" UINT64_FORMAT "|" UINT64_FORMAT "|"
					  UINT64_FORMAT "|%u|%u|%u|" UINT64_FORMAT "|"
					  UINT64_FORMAT "|%d|%d|%d|%d|%u|" UINT64_FORMAT "|"
					  UINT64_FORMAT "|%u|%u|%u|%d|%ld",
					  test_backend_runtime_protocol_park_state_name(snapshot.state),
					  test_backend_runtime_protocol_queue_state_name(
						  snapshot.scheduler_queue_state),
					  snapshot.generation,
					  snapshot.wake_reasons,
					  snapshot.wake_events,
					  snapshot.wake_generation,
					  snapshot.last_wake_reasons,
					  snapshot.last_wake_events,
					  snapshot.last_wake_generation,
					  snapshot.notify_wake_generation,
					  snapshot.deferred_notify_generation,
					  snapshot.deferred_notify_park_generation,
					  snapshot.deferred_notify_reasons,
					  snapshot.scheduler_runnable_count,
					  snapshot.scheduler_parked_protocol_count,
					  snapshot.scheduler_runnable_enqueue_count,
					  snapshot.scheduler_parked_protocol_enqueue_count,
					  snapshot.carrier_attached,
					  snapshot.session_present,
					  snapshot.connection_present,
					  snapshot.execution_present,
					  snapshot.scheduler_carrier_limit,
					  snapshot.scheduler_same_carrier_resume_count,
					  snapshot.scheduler_migrated_resume_count,
					  snapshot.scheduler_registered_carrier_count,
					  snapshot.scheduler_idle_carrier_count,
					  snapshot.scheduler_active_carrier_count,
					  snapshot.last_park_duration_valid,
					  snapshot.last_park_duration_ms);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
test_backend_runtime_wait_on_condition_variable(PG_FUNCTION_ARGS)
{
	int32		timeout_ms = PG_GETARG_INT32(0);
	ConditionVariable cv;
	volatile bool timed_out = false;

	if (timeout_ms < 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("condition variable wait timeout must not be negative"));

	if (test_backend_runtime_condition_variable_wait_event == 0)
		test_backend_runtime_condition_variable_wait_event =
			WaitEventExtensionNew("TestBackendRuntimeConditionVariable");

	ConditionVariableInit(&cv);
	ConditionVariablePrepareToSleep(&cv);

	PG_TRY();
	{
		timed_out = ConditionVariableTimedSleep(&cv, timeout_ms,
												test_backend_runtime_condition_variable_wait_event);
	}
	PG_FINALLY();
	{
		ConditionVariableCancelSleep();
	}
	PG_END_TRY();

	PG_RETURN_BOOL(timed_out);
}

Datum
test_backend_runtime_hold_lwlock(PG_FUNCTION_ARGS)
{
	int32		timeout_ms = PG_GETARG_INT32(0);
	volatile bool held = false;

	if (timeout_ms < 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("LWLock hold timeout must not be negative"));

	if (test_backend_runtime_hold_lwlock_wait_event == 0)
		test_backend_runtime_hold_lwlock_wait_event =
			WaitEventExtensionNew("TestBackendRuntimeHoldLWLock");

	LWLockAcquire(&test_backend_runtime_lwlock, LW_EXCLUSIVE);
	held = true;

	PG_TRY();
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 timeout_ms,
						 test_backend_runtime_hold_lwlock_wait_event);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	PG_FINALLY();
	{
		if (held)
			LWLockRelease(&test_backend_runtime_lwlock);
	}
	PG_END_TRY();

	PG_RETURN_BOOL(true);
}

Datum
test_backend_runtime_wait_on_lwlock(PG_FUNCTION_ARGS)
{
	LWLockAcquire(&test_backend_runtime_lwlock, LW_EXCLUSIVE);
	LWLockRelease(&test_backend_runtime_lwlock);

	PG_RETURN_BOOL(true);
}

void
test_backend_runtime_unreachable_bgworker_main(Datum main_arg)
{
	elog(FATAL, "process-model background worker unexpectedly started");
	pg_unreachable();
}

void
test_backend_runtime_thread_bgworker_main(Datum main_arg)
{
	if (test_backend_runtime_thread_bgworker_wait_event == 0)
		test_backend_runtime_thread_bgworker_wait_event =
			WaitEventExtensionNew("TestBackendRuntimeThreadBgWorker");

	for (;;)
	{
		(void) WaitLatch(MyLatch,
						 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 10000L,
						 test_backend_runtime_thread_bgworker_wait_event);
		ResetLatch(MyLatch);

		PgCurrentBackendApplyInterrupts();
		if (ProcDiePending || ShutdownRequestPending)
			break;

		CHECK_FOR_INTERRUPTS();
	}
}

void
test_backend_runtime_restart_bgworker_main(Datum main_arg)
{
	uint32		run_count;

	run_count = pg_atomic_fetch_add_u32(&test_backend_runtime_restart_count, 1) + 1;
	elog(LOG, "test_backend_runtime restart bgworker run %u", run_count);

	if (run_count == 1)
		proc_exit(1);

	test_backend_runtime_thread_bgworker_main(main_arg);
}

void
test_backend_runtime_crash_bgworker_main(Datum main_arg)
{
	uint32		run_count;

	run_count = pg_atomic_fetch_add_u32(&test_backend_runtime_crash_count, 1) + 1;
	elog(LOG, "test_backend_runtime crash bgworker run %u", run_count);

	proc_exit(17);
}
