/*-------------------------------------------------------------------------
 *
 * backend_runtime_teardown.c
 *	  Closed-backend/session/execution runtime teardown helpers.
 *
 * This file owns the ordered closed-state reset paths for runtime objects.
 * Keep root object construction, current-pointer installation, and early
 * fallback adoption in backend_runtime.c; move semantic destroy/reset logic
 * here or to a more specific subsystem-owned runtime file.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/backend/utils/init/backend_runtime_teardown.c
 *
 *-------------------------------------------------------------------------
 */
#define BACKEND_RUNTIME_NO_INLINE_BUCKET_ACCESSORS
#include "postgres.h"

#include "access/gin.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlogreader.h"
#include "archive/archive_module.h"
#include "commands/async.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/prepare.h"
#include "commands/trigger.h"
#include "lib/dshash.h"
#include "postmaster/pgarch.h"
#include "regex/regex.h"
#include "replication/reorderbuffer.h"
#include "replication/logical.h"
#include "replication/slotsync.h"
#include "replication/walreceiver.h"
#include "storage/bufmgr.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lock.h"
#include "storage/shm_mq.h"
#include "storage/waiteventset.h"
#include "tsearch/ts_cache.h"
#include "utils/backend_runtime.h"
#include "backend_runtime_internal.h"
#include "utils/dsa.h"
#include "utils/funccache.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/plancache.h"
#include "utils/resowner.h"
#include "utils/typcache.h"

static PgReusableSessionValidationReason PgValidateReusableSessionConnection(PgConnection *connection);
static PgReusableSessionValidationReason PgValidateReusableSessionBackend(PgBackend *backend);
static PgReusableSessionValidationReason PgValidateReusableSessionSession(PgSession *session);
static PgReusableSessionValidationReason PgValidateReusableSessionExecution(PgExecution *execution);
static bool PgConnectionCancelKeyIsZero(PgConnectionIdentityState *identity);
static bool PgBackendGlobalVisStateMatchesResetBaseline(struct GlobalVisState *state);
static bool PgBackendProcArrayStateIsReusable(PgBackend *backend);

static void
PgBackendResetStringInfo(StringInfoData *buf)
{
	if (buf == NULL)
		return;

	if (buf->data != NULL)
		pfree(buf->data);
	MemSet(buf, 0, sizeof(*buf));
}

PgReusableSessionValidationReason
PgValidateReusableSessionState(PgBackend *backend,
							   PgSession *session,
							   PgConnection *connection,
							   PgExecution *execution,
							   bool check_transaction_state)
{
	PgReusableSessionValidationReason reason;

	if (backend == NULL || session == NULL || connection == NULL ||
		execution == NULL)
		return PG_REUSABLE_SESSION_INVALID_NULL_OBJECT;

	if (check_transaction_state &&
		(IsTransactionOrTransactionBlock() ||
		 IsAbortedTransactionBlockState() ||
		 IsSubTransaction()))
		return PG_REUSABLE_SESSION_INVALID_TRANSACTION_ACTIVE;

	reason = PgValidateReusableSessionBackend(backend);
	if (reason != PG_REUSABLE_SESSION_VALID)
		return reason;

	reason = PgValidateReusableSessionSession(session);
	if (reason != PG_REUSABLE_SESSION_VALID)
		return reason;

	reason = PgValidateReusableSessionConnection(connection);
	if (reason != PG_REUSABLE_SESSION_VALID)
		return reason;

	reason = PgValidateReusableSessionExecution(execution);
	if (reason != PG_REUSABLE_SESSION_VALID)
		return reason;

	return PG_REUSABLE_SESSION_VALID;
}

const char *
PgReusableSessionValidationReasonName(PgReusableSessionValidationReason reason)
{
	switch (reason)
	{
		case PG_REUSABLE_SESSION_VALID:
			return "ok";
		case PG_REUSABLE_SESSION_INVALID_NULL_OBJECT:
			return "null_object";
		case PG_REUSABLE_SESSION_INVALID_TRANSACTION_ACTIVE:
			return "transaction_active";
		case PG_REUSABLE_SESSION_INVALID_PROC_ATTACHED:
			return "proc_attached";
		case PG_REUSABLE_SESSION_INVALID_PGSTAT_STATE:
			return "pgstat_state";
		case PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE:
			return "procarray_state";
		case PG_REUSABLE_SESSION_INVALID_IPC_STATE:
			return "ipc_state";
		case PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED:
			return "socket_attached";
		case PG_REUSABLE_SESSION_INVALID_PREPARED_STATEMENTS:
			return "prepared_statements";
		case PG_REUSABLE_SESSION_INVALID_PORTALS:
			return "portals";
		case PG_REUSABLE_SESSION_INVALID_LISTEN:
			return "listen";
		case PG_REUSABLE_SESSION_INVALID_TEMP_NAMESPACE:
			return "temp_namespace";
		case PG_REUSABLE_SESSION_INVALID_EXTENSION_STATE:
			return "extension_state";
		case PG_REUSABLE_SESSION_INVALID_DSM_SEGMENTS:
			return "dsm_segments";
		case PG_REUSABLE_SESSION_INVALID_RESOURCE_OWNER:
			return "resource_owner";
		case PG_REUSABLE_SESSION_INVALID_MEMORY_CONTEXTS:
			return "memory_contexts";
		case PG_REUSABLE_SESSION_INVALID_ACTIVE_TIMEOUTS:
			return "active_timeouts";
		case PG_REUSABLE_SESSION_INVALID_LOCKS:
			return "locks";
		case PG_REUSABLE_SESSION_INVALID_BUFFER_PINS:
			return "buffer_pins";
		case PG_REUSABLE_SESSION_INVALID_TEMP_FILES:
			return "temp_files";
		case PG_REUSABLE_SESSION_INVALID_GUC_STATE:
			return "guc_state";
		case PG_REUSABLE_SESSION_INVALID_PLAN_CACHE:
			return "plan_cache";
		case PG_REUSABLE_SESSION_INVALID_SNAPSHOTS:
			return "snapshots";
		case PG_REUSABLE_SESSION_INVALID_INVALIDATIONS:
			return "invalidations";
		case PG_REUSABLE_SESSION_INVALID_STORAGE_STATE:
			return "storage_state";
		case PG_REUSABLE_SESSION_INVALID_XLOG_INSERT_STATE:
			return "xlog_insert_state";
		case PG_REUSABLE_SESSION_INVALID_ASYNC_ACTIONS:
			return "async_actions";
		case PG_REUSABLE_SESSION_INVALID_REASON_COUNT:
			break;
	}

	return "unknown";
}

static PgReusableSessionValidationReason
PgValidateReusableSessionBackend(PgBackend *backend)
{
	Assert(backend != NULL);

	if (backend->my_proc != NULL ||
		backend->my_proc_number != INVALID_PROC_NUMBER ||
		backend->parallel_leader_proc_number != INVALID_PROC_NUMBER ||
		backend->aux_process_resource_owner != NULL)
		return PG_REUSABLE_SESSION_INVALID_PROC_ATTACHED;

	if (!dlist_is_empty(&backend->dsm_segment_list))
		return PG_REUSABLE_SESSION_INVALID_DSM_SEGMENTS;

	if (backend->my_beentry != NULL ||
		backend->my_bgworker_entry != NULL ||
		backend->activity.backend_status_table != NULL ||
		backend->activity.num_backends != 0 ||
		backend->activity.backend_status_context != NULL ||
		backend->pgstat_pending.local != NULL ||
		backend->pgstat_pending.fixed_snapshot_context != NULL ||
		backend->pgstat_pending.entry_ref_hash != NULL ||
		backend->pgstat_pending.shared_ref_age != 0 ||
		backend->pgstat_pending.shared_ref_context != NULL ||
		backend->pgstat_pending.entry_ref_hash_context != NULL ||
		backend->pgstat_pending.io_stats_pending ||
		backend->pgstat_pending.slru_stats_pending ||
		backend->pgstat_pending.lock_stats_pending ||
		backend->pgstat_pending.backend_io_stats_pending ||
		backend->pgstat_pending.cold != NULL ||
		backend->pgstat_pending.pending_context != NULL ||
		!dlist_is_empty(&backend->pgstat_pending.pending) ||
		backend->pgstat_pending.report_fixed ||
		backend->pgstat_pending.force_next_flush ||
		backend->pgstat_pending.force_snapshot_clear ||
		backend->pgstat_pending.is_initialized ||
		backend->pgstat_pending.is_shutdown)
		return PG_REUSABLE_SESSION_INVALID_PGSTAT_STATE;

	if (backend->ipc.proc_signal_slot != NULL ||
		backend->ipc.shared_invalid_message_counter != 0 ||
		backend->ipc.catchup_interrupt_pending ||
		backend->ipc.shared_invalidation_messages != NULL ||
		backend->ipc.shared_invalidation_next_msg != 0 ||
		backend->ipc.shared_invalidation_num_msgs != 0 ||
		backend->ipc.latch_wait_set != NULL ||
		pg_atomic_read_u32(&backend->interrupts.pending_mask) != 0 ||
		backend->interrupts.proc_die_sender_pid != 0 ||
		backend->interrupts.proc_die_sender_uid != 0)
		return PG_REUSABLE_SESSION_INVALID_IPC_STATE;

	if (backend->ipc.dsm_init_done ||
		backend->ipc.dsm_registry_dsa != NULL ||
		backend->ipc.dsm_registry_table != NULL)
		return PG_REUSABLE_SESSION_INVALID_DSM_SEGMENTS;

	if (!PgBackendProcArrayStateIsReusable(backend))
		return PG_REUSABLE_SESSION_INVALID_PROCARRAY_STATE;

	if (backend->timeout.num_active_timeouts != 0)
		return PG_REUSABLE_SESSION_INVALID_ACTIVE_TIMEOUTS;

	if (!LockManagerStateIsReusable(&backend->locks))
		return PG_REUSABLE_SESSION_INVALID_LOCKS;

	if (!BufferManagerPrivateRefCountStateIsReusable(&backend->buffers))
		return PG_REUSABLE_SESSION_INVALID_BUFFER_PINS;

	if (!FileAccessStateIsReusable(&backend->storage) ||
		backend->storage.sync_pending_ops != NULL ||
		backend->storage.sync_pending_unlinks != NIL ||
		backend->storage.sync_in_progress ||
		!dlist_is_empty(&backend->storage.smgr_unpinned_relations))
		return PG_REUSABLE_SESSION_INVALID_STORAGE_STATE;

	return PG_REUSABLE_SESSION_VALID;
}

static bool
PgBackendGlobalVisStateMatchesResetBaseline(struct GlobalVisState *state)
{
	Assert(state != NULL);

	return FullTransactionIdEquals(state->definitely_needed,
								   InvalidFullTransactionId) &&
		FullTransactionIdEquals(state->maybe_needed,
								InvalidFullTransactionId);
}

static bool
PgBackendProcArrayStateIsReusable(PgBackend *backend)
{
	PgBackendTransactionState *transaction;

	Assert(backend != NULL);

	transaction = &backend->transaction;
	if (backend->ipc.next_local_transaction_id != InvalidLocalTransactionId)
		return false;

	if (TransactionIdIsValid(transaction->procarray_cached_xid_not_in_progress) ||
		TransactionIdIsValid(transaction->compute_xid_horizons_result_last_xmin) ||
		!PgBackendGlobalVisStateMatchesResetBaseline(&transaction->global_vis_shared_rels) ||
		!PgBackendGlobalVisStateMatchesResetBaseline(&transaction->global_vis_catalog_rels) ||
		!PgBackendGlobalVisStateMatchesResetBaseline(&transaction->global_vis_data_rels) ||
		!PgBackendGlobalVisStateMatchesResetBaseline(&transaction->global_vis_temp_rels))
		return false;

	return true;
}

static PgReusableSessionValidationReason
PgValidateReusableSessionSession(PgSession *session)
{
	Assert(session != NULL);

	if (session->prepared_statement.prepared_queries != NULL)
		return PG_REUSABLE_SESSION_INVALID_PREPARED_STATEMENTS;

	if (session->portal_manager.portal_hash_table != NULL ||
		session->portal_manager.unnamed_portal_count != 0)
		return PG_REUSABLE_SESSION_INVALID_PORTALS;

	if (session->async.registered_listener ||
		session->async.local_channel_table != NULL)
		return PG_REUSABLE_SESSION_INVALID_LISTEN;

	if (OidIsValid(session->namespace_state.my_temp_namespace) ||
		OidIsValid(session->namespace_state.my_temp_toast_namespace) ||
		session->namespace_state.my_temp_namespace_subid != InvalidSubTransactionId)
		return PG_REUSABLE_SESSION_INVALID_TEMP_NAMESPACE;

	if (session->extension_modules.private_states != NIL ||
		session->extension_modules.reset_callbacks != NIL)
		return PG_REUSABLE_SESSION_INVALID_EXTENSION_STATE;

	if (session->temp_file.initialized &&
		(session->temp_file.temporary_files_size != 0 ||
		 session->temp_file.temp_table_spaces != NULL ||
		 session->temp_file.num_temp_table_spaces != -1 ||
		 session->temp_file.next_temp_table_space != 0))
		return PG_REUSABLE_SESSION_INVALID_TEMP_FILES;

	if (session->guc.initialized &&
		(session->guc.nest_level != 0 ||
		 !slist_is_empty(&session->guc.stack_list) ||
		 !slist_is_empty(&session->guc.report_list)))
		return PG_REUSABLE_SESSION_INVALID_GUC_STATE;
	if (session == CurrentPgSession)
	{
		if (!GUCStateMatchesResetBaseline())
			return PG_REUSABLE_SESSION_INVALID_GUC_STATE;
	}
	else if (session->guc.initialized &&
			 !dlist_is_empty(&session->guc.nondef_list))
		return PG_REUSABLE_SESSION_INVALID_GUC_STATE;

	if (session->plan_cache.initialized &&
		(!dlist_is_empty(&session->plan_cache.saved_plan_list) ||
		 !dlist_is_empty(&session->plan_cache.cached_expression_list)))
		return PG_REUSABLE_SESSION_INVALID_PLAN_CACHE;

	if (session->invalidation_callbacks.syscache_callback_count != 0 ||
		session->invalidation_callbacks.relcache_callback_count != 0 ||
		session->invalidation_callbacks.relsync_callback_count != 0)
		return PG_REUSABLE_SESSION_INVALID_INVALIDATIONS;

	return PG_REUSABLE_SESSION_VALID;
}

static PgReusableSessionValidationReason
PgValidateReusableSessionConnection(PgConnection *connection)
{
	Assert(connection != NULL);

	if (connection->identity.port != NULL ||
		connection->identity.port_context != NULL ||
		connection->identity.cancel_key_length != 0 ||
		!PgConnectionCancelKeyIsZero(&connection->identity))
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	if (connection->socket_io.send_buffer != NULL ||
		connection->socket_io.recv_buffer != NULL ||
		connection->socket_io.socket_io_context != NULL ||
		connection->socket_io.send_buffer_size != 0 ||
		connection->socket_io.send_pointer != 0 ||
		connection->socket_io.send_start != 0 ||
		connection->socket_io.recv_pointer != 0 ||
		connection->socket_io.recv_length != 0 ||
		connection->socket_io.comm_busy ||
		connection->socket_io.comm_reading_msg ||
		connection->socket_io.win32_noblock != 0 ||
		connection->socket_io.transport_generation != 0)
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	if (connection->protocol.comm_methods != NULL ||
		connection->protocol.fe_be_wait_set != NULL ||
		connection->protocol.frontend_protocol != 0)
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	if (connection->startup.client_auth_in_progress ||
		connection->startup.client_socket != NULL ||
		connection->startup.connection_warnings_emitted ||
		connection->startup.connection_warning_context != NULL ||
		connection->startup.connection_warning_messages != NIL ||
		connection->startup.connection_warning_details != NIL)
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	if (connection->client_connection_info.authn_id != NULL ||
		connection->client_connection_info.auth_method != uaReject ||
		connection->client_connection_info_context != NULL ||
		connection->client_connection_info_authn_id_owned)
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	if (connection->security.ssl_loaded_verify_locations ||
		connection->security.gss_send_buffer != NULL ||
		connection->security.gss_send_length != 0 ||
		connection->security.gss_send_next != 0 ||
		connection->security.gss_send_consumed != 0 ||
		connection->security.gss_recv_buffer != NULL ||
		connection->security.gss_recv_length != 0 ||
		connection->security.gss_result_buffer != NULL ||
		connection->security.gss_result_length != 0 ||
		connection->security.gss_result_next != 0 ||
		connection->security.gss_max_packet_size != 0 ||
		connection->security.pam_password != NULL ||
		connection->security.pam_port != NULL ||
		connection->security.pam_no_password)
		return PG_REUSABLE_SESSION_INVALID_SOCKET_ATTACHED;

	return PG_REUSABLE_SESSION_VALID;
}

static bool
PgConnectionCancelKeyIsZero(PgConnectionIdentityState *identity)
{
	int			i;

	Assert(identity != NULL);

	for (i = 0; i < PG_CONNECTION_CANCEL_KEY_LENGTH; i++)
	{
		if (identity->cancel_key[i] != 0)
			return false;
	}

	return true;
}

static PgReusableSessionValidationReason
PgValidateReusableSessionExecution(PgExecution *execution)
{
	Assert(execution != NULL);

	if (execution->resource_owners.current_owner != NULL ||
		execution->resource_owners.cur_transaction_owner != NULL ||
		execution->resource_owners.top_transaction_owner != NULL ||
		execution->resource_owners.resource_owner_context != NULL)
		return PG_REUSABLE_SESSION_INVALID_RESOURCE_OWNER;

	if (execution->memory_contexts.top_context != NULL ||
		execution->memory_contexts.current_context != NULL ||
		execution->memory_contexts.message_context != NULL)
		return PG_REUSABLE_SESSION_INVALID_MEMORY_CONTEXTS;

	if (execution->extension.private_states != NIL)
		return PG_REUSABLE_SESSION_INVALID_EXTENSION_STATE;

	if (execution->snapshot.current_snapshot != NULL ||
		execution->snapshot.secondary_snapshot != NULL ||
		execution->snapshot.catalog_snapshot != NULL ||
		execution->snapshot.historic_snapshot != NULL ||
		execution->snapshot.tuplecid_data != NULL ||
		execution->snapshot.active_snapshot != NULL ||
		!pairingheap_is_empty(&execution->snapshot.registered_snapshots) ||
		execution->snapshot.first_snapshot_set ||
		execution->snapshot.first_xact_snapshot != NULL ||
		execution->snapshot.exported_snapshots != NIL ||
		execution->combo_cid.hash != NULL ||
		execution->combo_cid.cids != NULL ||
		execution->combo_cid.used != 0 ||
		execution->combo_cid.size != 0)
		return PG_REUSABLE_SESSION_INVALID_SNAPSHOTS;

	if (execution->invalidation.message_arrays[0].msgs != NULL ||
		execution->invalidation.message_arrays[0].maxmsgs != 0 ||
		execution->invalidation.message_arrays[1].msgs != NULL ||
		execution->invalidation.message_arrays[1].maxmsgs != 0 ||
		execution->invalidation.trans_info != NULL ||
		execution->invalidation.inplace_info != NULL ||
		execution->catalog.pending_rel_deletes != NULL ||
		execution->catalog.pending_sync_hash != NULL)
		return PG_REUSABLE_SESSION_INVALID_INVALIDATIONS;

	if (execution->xloginsert.begininsert_called ||
		execution->xloginsert.max_registered_block_id != 0 ||
		execution->xloginsert.mainrdata_len != 0 ||
		execution->xloginsert.num_rdatas != 0 ||
		execution->xloginsert.curinsert_flags != 0)
		return PG_REUSABLE_SESSION_INVALID_XLOG_INSERT_STATE;

	if (execution->async.pending_actions != NULL ||
		execution->async.pending_listen_actions != NULL ||
		execution->async.pending_notifies != NULL ||
		execution->async.try_advance_tail)
		return PG_REUSABLE_SESSION_INVALID_ASYNC_ACTIONS;

	return PG_REUSABLE_SESSION_VALID;
}

static bool
PgBackendDsmSegmentListDrained(void)
{
	return CurrentPgBackend != NULL &&
		dlist_is_empty(&CurrentPgBackend->dsm_segment_list);
}

static void
PgBackendDetachDsaArea(dsa_area *area)
{
	if (area == NULL)
		return;

	/*
	 * shmem_exit() drains DSM mappings before final runtime bucket reset.  At
	 * that point DSA on-detach callbacks have already run and dsm_segment
	 * descriptors have been freed, so only the backend-local dsa_area wrapper
	 * remains safe to release here.
	 */
	if (PgBackendDsmSegmentListDrained())
		pfree(area);
	else
		dsa_detach(area);
}

static void
PgBackendDetachDsmSegment(dsm_segment *seg)
{
	if (seg == NULL)
		return;

	if (!PgBackendDsmSegmentListDrained())
		dsm_detach(seg);
}

static void
PgBackendResetExprInterpClosedState(PgBackendExprInterpState *expr_interp)
{
	Assert(expr_interp != NULL);

	if (expr_interp->reverse_dispatch_table != NULL)
		pfree(expr_interp->reverse_dispatch_table);
	MemSet(expr_interp, 0, sizeof(*expr_interp));
}

static void
PgBackendResetLockClosedState(PgBackendLockState *locks)
{
	Assert(locks != NULL);

	if (locks->held_lwlocks_array != NULL &&
		locks->held_lwlocks_array != locks->held_lwlocks_inline)
		pfree(locks->held_lwlocks_array);

	if (locks->fast_path_local_use_counts_owned &&
		locks->fast_path_local_use_counts != NULL)
		pfree(locks->fast_path_local_use_counts);

	if (locks->deadlock_workspace_owned)
	{
		if (locks->deadlock_visited_procs != NULL)
			pfree(locks->deadlock_visited_procs);
		if (locks->deadlock_before_constraints != NULL)
			pfree(locks->deadlock_before_constraints);
		if (locks->deadlock_after_constraints != NULL)
			pfree(locks->deadlock_after_constraints);
		if (locks->deadlock_wait_orders != NULL)
			pfree(locks->deadlock_wait_orders);
		if (locks->deadlock_wait_order_procs != NULL)
			pfree(locks->deadlock_wait_order_procs);
		if (locks->deadlock_cur_constraints != NULL)
			pfree(locks->deadlock_cur_constraints);
		if (locks->deadlock_possible_constraints != NULL)
			pfree(locks->deadlock_possible_constraints);
		if (locks->deadlock_details != NULL)
			pfree(locks->deadlock_details);
	}

	/*
	 * LWLOCK_STATS registers print_lwlock_stats() as a shmem-exit callback.
	 * proc_exit() drains that callback stack before closed-backend reset; this
	 * only reclaims any retained per-backend stats hash storage afterwards.
	 */
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(locks->lwlock_stats_context);
	locks->lwlock_stats_htab = NULL;
	locks->lwlock_stats_exit_registered = false;

	PgBackendInitializeLockState(locks);
}

static void
PgBackendResetExtensionModuleClosedState(PgBackendExtensionModuleState *extension_modules)
{
	Assert(extension_modules != NULL);

	foreach_ptr(PgBackendExtensionPrivateState, private_state,
				extension_modules->private_states)
	{
		if (private_state->cleanup != NULL &&
			private_state->state != NULL)
			private_state->cleanup(private_state->state);
	}

	foreach_ptr(PgBackendExtensionPrivateState, private_state,
				extension_modules->private_states)
	{
		if (private_state->state != NULL)
			pfree(private_state->state);
	}
	list_free_deep(extension_modules->private_states);

	PgBackendInitializeExtensionModuleState(extension_modules);
}

static void
PgBackendResetParallelClosedState(PgBackendParallelState *parallel)
{
	Assert(parallel != NULL);

	if (parallel->context_list_initialized)
		Assert(dlist_is_empty(&parallel->context_list));

	if (parallel->pq_mq_handle != NULL)
	{
		shm_mq_detach((shm_mq_handle *) parallel->pq_mq_handle);
		parallel->pq_mq_handle = NULL;
	}

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(parallel->message_context);

	PgBackendInitializeParallelState(parallel);
}

static void
PgBackendResetPgStatPendingClosedState(PgBackendPgStatPendingState *pgstat_pending)
{
	Assert(pgstat_pending != NULL);
	Assert(pgstat_pending->entry_ref_hash == NULL);
	if (pgstat_pending->local != NULL)
	{
		Assert(pgstat_pending->local->shared_hash == NULL);
		Assert(pgstat_pending->local->dsa == NULL);
	}

	/*
	 * Normal pgstat shutdown owns flushing, shared-entry release, and DSA
	 * detach.  Closed-backend reset only reclaims retained local contexts and
	 * restores constructor defaults for reuse.
	 */
	dlist_init(&pgstat_pending->pending);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(pgstat_pending->fixed_snapshot_context);
	if (pgstat_pending->local != NULL &&
		pgstat_pending->local->snapshot != NULL)
	{
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(pgstat_pending->local->snapshot->context);
		pfree(pgstat_pending->local->snapshot);
		pgstat_pending->local->snapshot = NULL;
	}
	if (pgstat_pending->local != NULL)
	{
		pfree(pgstat_pending->local);
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(pgstat_pending->shared_ref_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(pgstat_pending->entry_ref_hash_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(pgstat_pending->pending_context);
	if (pgstat_pending->cold != NULL)
	{
		free(pgstat_pending->cold);
	}

	PgBackendInitializePgStatPendingState(pgstat_pending);
}

static void
PgBackendResetWaitClosedState(PgBackendWaitState *wait_state)
{
	Assert(wait_state != NULL);

	PgBackendInitializeWaitState(wait_state);
}

static void
PgBackendResetBufferClosedState(PgBackendBufferState *buffers)
{
	Assert(buffers != NULL);
	Assert(buffers->n_local_pinned_buffers == 0);
	Assert(buffers->private_ref_count_overflowed == 0);

	/*
	 * During process-mode proc_exit(), normal buffer callbacks have already
	 * checked semantic cleanup, and some context-owned buffer helper storage
	 * can already be invalid.  Process exit will reclaim it.  Threaded client
	 * backend proc_exit() reuses this reset path without process teardown, so
	 * it must reclaim local-buffer arrays allocated with calloc(); context-
	 * owned buffer helper storage remains under the retained TopMemoryContext
	 * and is reclaimed when backend_thread_finish() deletes that root.
	 */
	if (PgBackendExitInProgress())
	{
		if (PgRuntimeIsThreadBacked(CurrentPgRuntime) &&
			CurrentPgBackend != NULL &&
			CurrentPgBackend->backend_type == B_BACKEND)
		{
			if (buffers->local_buffer_descriptors != NULL)
				free(buffers->local_buffer_descriptors);
			if (buffers->local_buffer_block_pointers != NULL)
				free(buffers->local_buffer_block_pointers);
			if (buffers->local_ref_count != NULL)
				free(buffers->local_ref_count);
		}

		PgBackendInitializeBufferState(buffers);
		return;
	}

	if (buffers->local_buffer_descriptors != NULL)
		free(buffers->local_buffer_descriptors);
	if (buffers->local_buffer_block_pointers != NULL)
		free(buffers->local_buffer_block_pointers);
	if (buffers->local_ref_count != NULL)
		free(buffers->local_ref_count);

	PG_RUNTIME_DESTROY_HASH(buffers->local_buf_hash);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(buffers->local_buffer_context);

	if (buffers->backend_writeback_context != NULL)
		pfree(buffers->backend_writeback_context);

	if (buffers->private_ref_count_array_keys != NULL)
		pfree(buffers->private_ref_count_array_keys);
	if (buffers->private_ref_count_array != NULL)
		pfree(buffers->private_ref_count_array);
	PG_RUNTIME_DESTROY_HASH(buffers->private_ref_count_hash);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(buffers->buffer_context);

	PgBackendInitializeBufferState(buffers);
}

static void
PgBackendResetIPCWaitSetClosedState(PgBackendIPCState *ipc)
{
	Assert(ipc != NULL);

	if (ipc->latch_wait_set != NULL)
	{
		FreeWaitEventSet(ipc->latch_wait_set);
		ipc->latch_wait_set = NULL;
	}
}

static void
PgBackendResetIPCClosedState(PgBackendIPCState *ipc)
{
	Assert(ipc != NULL);

	if (ipc->dsm_registry_table != NULL)
		dshash_detach((dshash_table *) ipc->dsm_registry_table);
	if (ipc->dsm_registry_dsa != NULL)
		PgBackendDetachDsaArea((dsa_area *) ipc->dsm_registry_dsa);
	if (ipc->latch_wait_set != NULL)
		FreeWaitEventSet(ipc->latch_wait_set);

	PgBackendInitializeIPCState(ipc);
}

static void
PgBackendResetTransactionClosedState(PgBackendTransactionState *transaction)
{
	Assert(transaction != NULL);
	Assert(!transaction->multixact_cache_initialized ||
		   dclist_is_empty(&transaction->multixact_cache));

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(transaction->multixact_context);
	if (transaction->multixact_debug_string != NULL)
		pfree(transaction->multixact_debug_string);

	PgBackendInitializeTransactionState(transaction);
}

static void
PgBackendResetRecoveryClosedState(PgBackendRecoveryState *recovery)
{
	Assert(recovery != NULL);

	PG_RUNTIME_DESTROY_HASH(recovery->recovery_lock_hash);
	PG_RUNTIME_DESTROY_HASH(recovery->recovery_lock_xid_hash);
	if (recovery->startup_observed_primary_conninfo != NULL)
		pfree(recovery->startup_observed_primary_conninfo);
	if (recovery->startup_observed_primary_slotname != NULL)
		pfree(recovery->startup_observed_primary_slotname);

	PgBackendInitializeRecoveryState(recovery);
}

static void
PgBackendResetRepackClosedState(PgBackendRepackState *repack)
{
	Assert(repack != NULL);
	Assert(repack->decoding_worker == NULL);

	if (repack->worker_dsm_segment != NULL)
		PgBackendDetachDsmSegment(repack->worker_dsm_segment);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(repack->message_context);

	PgBackendInitializeRepackState(repack);
}

static void
PgExecutionResetErrorClosedState(PgExecutionErrorState *error)
{
	PgExecutionInitializeErrorState(error);
}

static void
PgExecutionResetResourceOwnersClosedState(PgExecutionResourceOwnerState
										  *resource_owners)
{
	bool		have_live_owner;
	MemoryContext resource_owner_context;

	Assert(resource_owners != NULL);

	have_live_owner = resource_owners->current_owner != NULL ||
		resource_owners->cur_transaction_owner != NULL ||
		resource_owners->top_transaction_owner != NULL;
	resource_owner_context = resource_owners->resource_owner_context;

	resource_owners->current_owner = NULL;
	resource_owners->cur_transaction_owner = NULL;
	resource_owners->top_transaction_owner = NULL;

	if (!have_live_owner && resource_owner_context != NULL)
	{
		MemoryContextDelete(resource_owner_context);
		resource_owners->resource_owner_context = NULL;
	}
}

static void
PgExecutionResetSPIClosedState(PgExecutionSPIState *spi)
{
	PgExecutionInitializeSPIState(spi);
}

static void
PgExecutionResetSnapshotDataArrays(SnapshotData *snapshot)
{
	Assert(snapshot != NULL);

	if (snapshot->xip != NULL)
		free(snapshot->xip);
	if (snapshot->subxip != NULL)
		free(snapshot->subxip);
	snapshot->xip = NULL;
	snapshot->subxip = NULL;
}

static void
PgExecutionResetSnapshotClosedState(PgExecutionSnapshotState *snapshot)
{
	Assert(snapshot != NULL);

	/*
	 * GetSnapshotData() mallocs xip/subxip arrays and relies on process exit
	 * to reclaim them because the historical SnapshotData objects are static.
	 * Threaded logical backends embed those SnapshotData objects in
	 * PgExecution, so connection churn must release the arrays explicitly.
	 */
	PgExecutionResetSnapshotDataArrays(&snapshot->current_snapshot_data);
	PgExecutionResetSnapshotDataArrays(&snapshot->secondary_snapshot_data);
	PgExecutionResetSnapshotDataArrays(&snapshot->catalog_snapshot_data);

	PgExecutionInitializeSnapshotState(snapshot);
}

static void
PgExecutionResetPortalClosedState(PgExecutionPortalState *portal)
{
	Assert(portal != NULL);

	MemSet(portal, 0, sizeof(*portal));
}

static void
PgExecutionResetReplicationScratchClosedState(PgExecutionReplicationScratchState
											  *replication_scratch)
{
	Assert(replication_scratch != NULL);

	EventTriggerResetQueryStateStack(&replication_scratch->event_trigger_query_state);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(replication_scratch->event_trigger_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(replication_scratch->apply_message_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(replication_scratch->logical_streaming_context);
	PgExecutionInitializeReplicationScratchState(replication_scratch);
}

static void
PgBackendResetWalSenderClosedState(PgBackendWalSenderState *walsender)
{
	if (walsender == NULL)
		return;

	if (walsender->logical_decoding_ctx != NULL)
	{
		FreeDecodingContext(walsender->logical_decoding_ctx);
		walsender->logical_decoding_ctx = NULL;
		walsender->xlogreader = NULL;
	}
	else if (walsender->xlogreader != NULL)
	{
		XLogReaderFree(walsender->xlogreader);
		walsender->xlogreader = NULL;
	}

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(walsender->uploaded_manifest_mcxt);
	walsender->uploaded_manifest = NULL;

	PgBackendResetStringInfo(&walsender->output_message);
	PgBackendResetStringInfo(&walsender->reply_message);
	PgBackendResetStringInfo(&walsender->tmpbuf);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(walsender->replication_cmd_context);

	if (walsender->lag_tracker != NULL)
	{
		pfree(walsender->lag_tracker);
		walsender->lag_tracker = NULL;
	}

	MemSet(walsender, 0, sizeof(*walsender));
}

static void
PgBackendResetReplicationClosedState(PgBackendReplicationState *replication)
{
	if (replication == NULL)
		return;

	if (replication->walreceiver_conn != NULL)
	{
		walrcv_disconnect(replication->walreceiver_conn);
		replication->walreceiver_conn = NULL;
	}

	if (replication->walreceiver_recv_file >= 0)
	{
		(void) close(replication->walreceiver_recv_file);
		replication->walreceiver_recv_file = -1;
	}

	PgBackendResetStringInfo(&replication->walreceiver_reply_message);
}

static void
PgBackendResetLogicalReplicationClosedState(PgBackendLogicalReplicationState *logical_replication)
{
	if (logical_replication == NULL)
		return;

	if (logical_replication->logrep_worker_walrcv_conn != NULL)
	{
		walrcv_disconnect(logical_replication->logrep_worker_walrcv_conn);
		logical_replication->logrep_worker_walrcv_conn = NULL;
	}

	if (logical_replication->stream_fd != NULL)
	{
		BufFileClose(logical_replication->stream_fd);
		logical_replication->stream_fd = NULL;
	}

	if (logical_replication->copybuf != NULL)
	{
		/*
		 * Table-sync COPY stores walreceiver-owned buffers in copybuf->data;
		 * only the StringInfo wrapper belongs to this backend state.
		 */
		pfree(logical_replication->copybuf);
		logical_replication->copybuf = NULL;
	}

	if (logical_replication->subxact_data.subxacts != NULL)
	{
		pfree(logical_replication->subxact_data.subxacts);
		logical_replication->subxact_data.subxacts = NULL;
	}
	logical_replication->subxact_data.nsubxacts = 0;
	logical_replication->subxact_data.nsubxacts_max = 0;
	logical_replication->subxact_data.subxact_last = InvalidTransactionId;

	if (logical_replication->apply_error_callback_arg.origin_name != NULL)
	{
		pfree(logical_replication->apply_error_callback_arg.origin_name);
		logical_replication->apply_error_callback_arg.origin_name = NULL;
	}
	logical_replication->apply_error_callback_arg.rel = NULL;
	logical_replication->apply_error_callback_arg.remote_attnum = -1;
	logical_replication->apply_error_callback_arg.remote_xid = InvalidTransactionId;
	logical_replication->apply_error_callback_arg.finish_lsn = InvalidXLogRecPtr;

	if (logical_replication->slotsync_observed_primary_conninfo != NULL)
	{
		pfree(logical_replication->slotsync_observed_primary_conninfo);
		logical_replication->slotsync_observed_primary_conninfo = NULL;
	}
	if (logical_replication->slotsync_observed_primary_slotname != NULL)
	{
		pfree(logical_replication->slotsync_observed_primary_slotname);
		logical_replication->slotsync_observed_primary_slotname = NULL;
	}

	PG_RUNTIME_DESTROY_HASH(logical_replication->table_sync_last_start_times);
	PG_RUNTIME_DESTROY_HASH(logical_replication->parallel_apply_txn_hash);

	PG_RUNTIME_LIST_FREE(logical_replication->on_commit_wakeup_workers_subids);
	PG_RUNTIME_LIST_FREE(logical_replication->table_states_not_ready);
	PG_RUNTIME_LIST_FREE(logical_replication->seqinfos);
	PG_RUNTIME_LIST_FREE(logical_replication->parallel_apply_worker_pool);
	PG_RUNTIME_LIST_FREE(logical_replication->parallel_apply_subxactlist);

	/*
	 * ApplyMessageContext and LogicalStreamingContext live in execution
	 * scratch state, but logical apply workers create them below ApplyContext.
	 * Backend closed-state reset runs before execution reset during threaded
	 * proc_exit(), so deleting ApplyContext here also deletes those children.
	 * Clear the execution-owned aliases before the later execution reset sees
	 * stale context headers.
	 */
	if (logical_replication->apply_context != NULL &&
		CurrentPgExecution != NULL)
	{
		CurrentPgExecution->replication_scratch.apply_message_context = NULL;
		CurrentPgExecution->replication_scratch.logical_streaming_context = NULL;
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(logical_replication->apply_context);

	dlist_init(&logical_replication->lsn_mapping);
	logical_replication->my_parallel_shared = NULL;
	logical_replication->my_subscription = NULL;
	logical_replication->my_subscription_valid = false;
	logical_replication->my_logical_rep_worker = NULL;
	logical_replication->on_commit_wakeup_workers_subids = NIL;
	logical_replication->table_states_not_ready = NIL;
	logical_replication->syncing_relations_has_subtables = false;
	logical_replication->syncing_relations_has_subsequences_non_ready = false;
	logical_replication->feedback_reply_message = NULL;
	logical_replication->feedback_send_time = 0;
	logical_replication->feedback_last_recvpos = InvalidXLogRecPtr;
	logical_replication->feedback_last_writepos = InvalidXLogRecPtr;
	logical_replication->status_request_message = NULL;
	logical_replication->seqinfos = NIL;
	if (logical_replication->launcher_last_start_times != NULL)
	{
		dshash_detach(logical_replication->launcher_last_start_times);
		logical_replication->launcher_last_start_times = NULL;
	}
	if (logical_replication->launcher_last_start_times_dsa != NULL)
	{
		PgBackendDetachDsaArea(logical_replication->launcher_last_start_times_dsa);
		logical_replication->launcher_last_start_times_dsa = NULL;
	}
	logical_replication->parallel_apply_worker_pool = NIL;
	logical_replication->stream_apply_worker = NULL;
	logical_replication->parallel_apply_subxactlist = NIL;
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		logical_replication->parallel_apply_message_context);
}

static void
PgBackendResetXLogClosedState(PgBackendXLogState *xlog)
{
	if (xlog == NULL)
		return;

	if (xlog->open_log_file >= 0)
	{
		(void) close(xlog->open_log_file);
		xlog->open_log_file = -1;
	}

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(xlog->wal_debug_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(xlog->btree_xlog_op_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(xlog->gin_xlog_op_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(xlog->gist_xlog_op_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(xlog->spgist_xlog_op_context);
}

static void
PgBackendResetMaintenanceWorkerClosedState(PgBackendMaintenanceWorkerState *maintenance_worker)
{
	if (maintenance_worker == NULL)
		return;

	/*
	 * arch_module_check_errdetail() returns storage owned by ErrorContext.
	 * Runtime reset may run after that context has been flushed, so this state
	 * only tracks the transient pointer and must not free it.
	 */
	maintenance_worker->arch_module_errdetail_string = NULL;
	if (maintenance_worker->archive_module_state != NULL)
	{
		pfree(maintenance_worker->archive_module_state);
		maintenance_worker->archive_module_state = NULL;
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(maintenance_worker->archive_context);
	if (maintenance_worker->loaded_archive_library != NULL)
	{
		pfree(maintenance_worker->loaded_archive_library);
		maintenance_worker->loaded_archive_library = NULL;
	}
	PgArchResetFilesState(&maintenance_worker->pgarch_files);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(maintenance_worker->bgwriter_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(maintenance_worker->walwriter_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(maintenance_worker->checkpointer_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(maintenance_worker->walsummarizer_context);

	maintenance_worker->archive_callbacks = NULL;
	maintenance_worker->checkpointer_shutdown_xlog_complete = false;
}

static void
PgBackendResetAutovacuumClosedState(PgBackendAutovacuumState *autovacuum)
{
	if (autovacuum == NULL)
		return;

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(autovacuum->autovac_mem_cxt);
	autovacuum->database_list_cxt = NULL;
	autovacuum->avl_dbase_array = NULL;
	autovacuum->my_worker_info = NULL;
	dlist_init(&autovacuum->database_list);
}

static void
PgBackendResetAioClosedState(PgBackendAioState *aio)
{
	if (aio == NULL)
		return;

	aio->my_backend = NULL;
	aio->my_io_worker_id = -1;
	aio->my_uring_context = NULL;
}

static void
PgBackendResetMemoryManagerClosedState(PgBackendMemoryManagerState *memory_manager)
{
	if (memory_manager == NULL)
		return;

	/*
	 * The AllocSet freelist is tied to memory-context ownership, not this
	 * bookkeeping bucket.  Process exit lets the operating system reclaim it.
	 * Threaded logical exit keeps the retained TopMemoryContext alive until
	 * the carrier finish handoff, and that handoff drains the per-backend
	 * freelists after deleting the retained root.  Leave the freelists intact
	 * here so all closed-state MemoryContextDelete() calls have one owner for
	 * final freelist reclamation.
	 */
	memory_manager->log_memory_context_in_progress = false;
}

static void
PgBackendResetUtilityClosedState(PgBackendUtilityState *utility)
{
	int			i;

	if (utility == NULL)
		return;

	utility->notify_interrupt_pending = false;

	if (utility->async_global_channel_table != NULL)
		dshash_detach(utility->async_global_channel_table);
	if (utility->async_global_channel_dsa != NULL)
		PgBackendDetachDsaArea(utility->async_global_channel_dsa);
	utility->async_global_channel_table = NULL;
	utility->async_global_channel_dsa = NULL;

	ResetExtensionSiblingCache();

	PG_RUNTIME_DESTROY_HASH(utility->injection_point_cache);

	for (i = 0; i < utility->num_seq_scans; i++)
	{
		utility->seq_scan_tables[i] = NULL;
		utility->seq_scan_levels[i] = 0;
	}
	utility->num_seq_scans = 0;

	ResetResourceReleaseCallbacks();

	for (i = 0; i < utility->n_dch_cache; i++)
	{
		pfree(utility->dch_cache[i]);
		utility->dch_cache[i] = NULL;
	}
	utility->n_dch_cache = 0;
	utility->dch_counter = 0;

	for (i = 0; i < utility->n_num_cache; i++)
	{
		pfree(utility->num_cache[i]);
		utility->num_cache[i] = NULL;
	}
	utility->n_num_cache = 0;
	utility->num_counter = 0;

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(utility->format_cache_context);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(utility->libxml_context);

	PG_RUNTIME_DESTROY_HASH(utility->missing_attr_cache);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(utility->utility_cache_context);
}

void
PgBackendResetClosedState(PgBackend *backend)
{
	if (backend == NULL)
		return;

	PgBackendUnregisterThreadedBackend(backend);

	/*
	 * The IPC bucket owns the backend latch wait set.  Freeing that wait set
	 * releases external FDs tracked by the storage bucket, so close only that
	 * wait set before the generated reset loop reaches storage.  Leave the rest
	 * of IPC state to the bucket's normal position so recovery and replication
	 * teardown keep their historical ordering.
	 */
	PgBackendResetIPCWaitSetClosedState(&backend->ipc);

#define PG_BACKEND_BUCKET(field, init, adopt, reset) \
	do { \
		reset; \
	} while (0);
#include "backend_runtime_backend_buckets.def"
#undef PG_BACKEND_BUCKET
}

static void
PgSessionResetTcopClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->tcop.unnamed_stmt_psrc != NULL)
	{
		CachedPlanSource *psrc = session->tcop.unnamed_stmt_psrc;

		session->tcop.unnamed_stmt_psrc = NULL;
		DropCachedPlan(psrc);
	}
	if (session->tcop.row_description_context != NULL)
	{
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->tcop.row_description_context);
		MemSet(&session->tcop.row_description_buf, 0,
			   sizeof(session->tcop.row_description_buf));
	}
}

static void
PgSessionResetPreparedStatementClosedState(PgSession *session)
{
	PgSession  *saved_session;

	Assert(session != NULL);

	if (session->prepared_statement.prepared_queries != NULL)
	{
		saved_session = CurrentPgSession;
		PgSetCurrentSession(session);
		PG_TRY();
		{
			DropAllPreparedStatements();
			PgSetCurrentSession(saved_session);
		}
		PG_CATCH();
		{
			PgSetCurrentSession(saved_session);
			PG_RE_THROW();
		}
		PG_END_TRY();
		hash_destroy(session->prepared_statement.prepared_queries);
		session->prepared_statement.prepared_queries = NULL;
	}
}

static void
PgSessionResetXactCallbackClosedState(PgSession *session)
{
	PgSession  *saved_session;

	Assert(session != NULL);

	saved_session = CurrentPgSession;
	PgSetCurrentSession(session);
	ResetXactCallbackState();
	PgSetCurrentSession(saved_session);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->xact_callbacks.xact_callback_context);
}

static void
PgSessionResetBackupClosedState(PgSession *session)
{
	PgSession  *saved_session;

	Assert(session != NULL);

	if (session->backup.session_backup_state != SESSION_BACKUP_NONE)
	{
		saved_session = CurrentPgSession;
		PgSetCurrentSession(session);
		PG_TRY();
		{
			do_pg_abort_backup(0, BoolGetDatum(false));
			PgSetCurrentSession(saved_session);
		}
		PG_CATCH();
		{
			PgSetCurrentSession(saved_session);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->backup.backup_context);
	session->backup.backup_state = NULL;
	session->backup.tablespace_map = NULL;
	session->backup.session_backup_state = SESSION_BACKUP_NONE;
	session->backup.abort_backup_handler_registered = false;
}

static void
PgSessionResetAsyncClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DESTROY_HASH(session->async.local_channel_table);
	session->async.registered_listener = false;
}

static void
PgSessionResetFunctionManagerClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DESTROY_HASH(session->function_manager.c_func_hash);
	if (session->function_manager.cached_function_hash != NULL)
	{
		DestroyCachedFunctionHash(session->function_manager.cached_function_hash);
		session->function_manager.cached_function_hash = NULL;
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->function_manager.function_manager_context);
}

static void
PgSessionResetExtensionModuleClosedState(PgSession *session)
{
	PgSession  *saved_session;

	Assert(session != NULL);

	saved_session = CurrentPgSession;
	PgSetCurrentSession(session);
	PG_TRY();
	{
		foreach_ptr(PgSessionResetCallbackItem, item,
					session->extension_modules.reset_callbacks)
			item->callback(item->arg);
		foreach_ptr(PgSessionExtensionPrivateState, private_state,
					session->extension_modules.private_states)
		{
			if (private_state->cleanup != NULL &&
				private_state->state != NULL)
				private_state->cleanup(private_state->state);
		}
		PgSetCurrentSession(saved_session);
	}
	PG_CATCH();
	{
		PgSetCurrentSession(saved_session);
		PG_RE_THROW();
	}
	PG_END_TRY();

	foreach_ptr(PgSessionExtensionPrivateState, private_state,
				session->extension_modules.private_states)
	{
		if (private_state->state != NULL)
			pfree(private_state->state);
	}
	list_free_deep(session->extension_modules.private_states);
	list_free_deep(session->extension_modules.reset_callbacks);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->extension_modules.plpython_memory_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->extension_modules.plperl_memory_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->extension_modules.pltcl_memory_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->extension_modules.plsample_memory_context);
	PgSessionInitializeExtensionModuleState(&session->extension_modules);
}

static void
PgSessionResetPgStatClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializePgStatState(&session->pgstat);
}

static void
PgSessionResetEncodingClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->encoding.encoding_cache_context);

	PgSessionInitializeEncodingState(&session->encoding);
}

static void
PgSessionResetInvalidationCallbackClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializeInvalidationCallbackState(&session->invalidation_callbacks);
}

static void
PgSessionResetRIGlobalsClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DESTROY_HASH(session->ri_globals.constraint_cache);
	PG_RUNTIME_DESTROY_HASH(session->ri_globals.query_cache);
	PG_RUNTIME_DESTROY_HASH(session->ri_globals.compare_cache);
	dclist_init(&session->ri_globals.constraint_cache_valid_list);
	session->ri_globals.fastpath_xact_callback_registered = false;
	session->ri_globals.debug_discard_caches_initialized = true;
	session->ri_globals.debug_discard_caches_value = DEFAULT_DEBUG_DISCARD_CACHES;
}

static void
PgSessionResetRelMapClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializeRelMapState(&session->relmap);
}

static void
PgSessionResetGUCClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (PgBackendExitInProgress())
		ResetGUCStateAtBackendExit();
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->guc.memory_context);
	PgSessionInitializeGUCState(&session->guc);
}

static void
PgSessionResetDateTimeClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (!PgBackendExitInProgress())
		return;

	session->datetime.timezone_abbrev_table = NULL;
	MemSet(session->datetime.timezone_abbrev_cache, 0,
		   sizeof(session->datetime.timezone_abbrev_cache));
}

static void
PgSessionResetLogicalReplicationClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->logical_replication.logical_rep_relmap_context != NULL)
	{
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->logical_replication.logical_rep_relmap_context);
		session->logical_replication.logical_rep_relmap = NULL;
	}
	else if (session->logical_replication.logical_rep_relmap != NULL)
	{
		PG_RUNTIME_DESTROY_HASH(session->logical_replication.logical_rep_relmap);
	}

	if (session->logical_replication.logical_rep_partmap_context != NULL)
	{
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->logical_replication.logical_rep_partmap_context);
		session->logical_replication.logical_rep_partmap = NULL;
	}
	else if (session->logical_replication.logical_rep_partmap != NULL)
	{
		PG_RUNTIME_DESTROY_HASH(session->logical_replication.logical_rep_partmap);
	}

	PG_RUNTIME_DESTROY_HASH(session->logical_replication.pgoutput_relation_sync_cache);
	session->logical_replication.session_replication_state = NULL;
	session->logical_replication.replication_origin_cleanup_registered = false;
	session->logical_replication.pgoutput_publications_valid = false;
	session->logical_replication.pgoutput_publication_callback_registered = false;
	session->logical_replication.pgoutput_relation_callbacks_registered = false;
	session->logical_replication.syncing_relations_state = 0;
}

static void
PgSessionResetUserIdentityClosedState(PgSession *session)
{
	Assert(session != NULL);

	for (int i = 0; i < lengthof(session->user_identity.cached_roles); i++)
	{
		session->user_identity.cached_role[i] = InvalidOid;
		PG_RUNTIME_LIST_FREE(session->user_identity.cached_roles[i]);
	}
	if (session->user_identity.system_user_owned &&
		session->user_identity.system_user != NULL &&
		session->user_identity.system_user_context == NULL)
		pfree((void *) session->user_identity.system_user);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->user_identity.system_user_context);
	session->user_identity.system_user = NULL;
	session->user_identity.system_user_owned = false;
	session->user_identity.cached_db_hash = 0;
}

static void
PgSessionResetTextSearchClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DESTROY_HASH(session->text_search.parser_cache_hash);
	session->text_search.last_used_parser = NULL;

	if (session->text_search.dictionary_cache_hash != NULL)
	{
		HASH_SEQ_STATUS status;
		TSDictionaryCacheEntry *entry;

		hash_seq_init(&status, session->text_search.dictionary_cache_hash);
		while ((entry = (TSDictionaryCacheEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->dictCtx != NULL)
			{
				MemoryContextDelete(entry->dictCtx);
				entry->dictCtx = NULL;
				entry->dictData = NULL;
			}
		}
		hash_destroy(session->text_search.dictionary_cache_hash);
		session->text_search.dictionary_cache_hash = NULL;
	}
	session->text_search.last_used_dictionary = NULL;

	if (session->text_search.config_cache_hash != NULL)
	{
		HASH_SEQ_STATUS status;
		TSConfigCacheEntry *entry;

		hash_seq_init(&status, session->text_search.config_cache_hash);
		while ((entry = (TSConfigCacheEntry *) hash_seq_search(&status)) != NULL)
		{
			if (entry->map != NULL)
			{
				for (int i = 0; i < entry->lenmap; i++)
				{
					if (entry->map[i].dictIds != NULL)
						pfree(entry->map[i].dictIds);
				}
				pfree(entry->map);
				entry->map = NULL;
				entry->lenmap = 0;
			}
		}
		hash_destroy(session->text_search.config_cache_hash);
		session->text_search.config_cache_hash = NULL;
	}
	session->text_search.last_used_config = NULL;
	session->text_search.current_config_cache = InvalidOid;
}

static void
PgSessionResetDatabaseClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->database.database_path != NULL)
	{
		if (session->database.database_path_owned &&
			session->database.database_path_context == NULL)
			pfree(session->database.database_path);
		session->database.database_path = NULL;
	}
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->database.database_path_context);
	session->database.database_path_owned = false;
}

static void
PgSessionResetDynamicLibraryInitsClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->dynamic_library_inits != NIL)
		list_free(session->dynamic_library_inits);

	session->dynamic_library_inits = NIL;
}

static void
PgSessionResetRegexClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->regex.regexp_cache_context);
	pg_free_regex_ctype_cache_list(session->regex.ctype_cache_list);
	PgSessionInitializeRegexState(&session->regex);
}

static void
PgSessionResetPortalManagerClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->portal_manager.top_portal_context);
	PgSessionInitializePortalManagerState(&session->portal_manager);
}

static void
PgSessionResetOptimizerClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->optimizer.planner_extension_names != NULL)
	{
		pfree(session->optimizer.planner_extension_names);
		session->optimizer.planner_extension_names = NULL;
	}
	session->optimizer.planner_extension_names_assigned = 0;
	session->optimizer.planner_extension_names_allocated = 0;
	PG_RUNTIME_DESTROY_HASH(session->optimizer.opr_proof_cache_hash);
}

static void
PgSessionResetLocaleClosedState(PgSession *session)
{
	Assert(session != NULL);

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->locale.locale_time_context);
	PgSessionResetLocaleTime(&session->locale);

	PgSessionResetLocaleConv(&session->locale);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->locale.locale_conv_context);

	if (PgBackendExitInProgress() &&
		session->locale.default_locale != NULL)
	{
		pg_locale_release_external((pg_locale_t) session->locale.default_locale);
		session->locale.default_locale = NULL;
	}

	if (session->locale.collation_cache_context != NULL)
	{
		if (PgBackendExitInProgress())
			pg_locale_release_collation_cache_external(session->locale.collation_cache);
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->locale.collation_cache_context);
		session->locale.collation_cache = NULL;
		session->locale.last_collation_cache_oid = InvalidOid;
		session->locale.last_collation_cache_locale = NULL;
	}
	if (session->locale.icu_converter != NULL)
	{
		PgCloseIcuConverter(session->locale.icu_converter);
		session->locale.icu_converter = NULL;
	}
}

static void
PgSessionResetLegacySessionContextClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (session->legacy_session_context != NULL)
	{
		if (CurrentPgSession == session &&
			CurrentSession == session->legacy_session)
			CurrentSession = NULL;
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->legacy_session_context);
	}
}

static void
PgSessionResetLegacySessionClosedState(PgSession *session)
{
	Assert(session != NULL);

	if (CurrentPgSession == session &&
		CurrentSession == session->legacy_session)
		CurrentSession = NULL;
	session->legacy_session = NULL;
}

static void
PgSessionResetVacuumClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializeVacuumState(&session->vacuum);
}

static void
PgSessionResetLockWaitClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializeLockWaitState(&session->lock_wait);
}

static void
PgSessionResetLargeObjectClosedState(PgSession *session)
{
	Assert(session != NULL);
	Assert(session->large_object.heap_relation == NULL);
	Assert(session->large_object.index_relation == NULL);

	PgSessionInitializeLargeObjectState(&session->large_object);
}

static void
PgSessionResetTempFileClosedState(PgSession *session)
{
	Assert(session != NULL);

	PgSessionInitializeTempFileState(&session->temp_file);
}

static void
PgSessionResetPlanCacheClosedState(PgSession *session)
{
	dlist_mutable_iter iter;
	bool		plan_cache_initialized;

	Assert(session != NULL);
	plan_cache_initialized = session->plan_cache.initialized;

	if (plan_cache_initialized)
	{
		dlist_foreach_modify(iter, &session->plan_cache.saved_plan_list)
		{
			CachedPlanSource *psrc;

			psrc = dlist_container(CachedPlanSource, node, iter.cur);
			DropCachedPlan(psrc);
		}

		dlist_foreach_modify(iter, &session->plan_cache.cached_expression_list)
		{
			CachedExpression *cexpr;

			cexpr = dlist_container(CachedExpression, node, iter.cur);
			FreeCachedExpression(cexpr);
		}

		Assert(dlist_is_empty(&session->plan_cache.saved_plan_list));
		Assert(dlist_is_empty(&session->plan_cache.cached_expression_list));
	}

	/*
	 * CacheMemoryContext belongs to catalog_lookup, but saved plan sources live
	 * under it.  Delete it only after the final plan-cache sweep has unlinked and
	 * dropped those sources.
	 */
	if (session->catalog_lookup.cache_memory_context != NULL)
	{
		if (CurrentMemoryContext == session->catalog_lookup.cache_memory_context)
			MemoryContextSwitchTo(TopMemoryContext);
		PG_RUNTIME_DELETE_MEMORY_CONTEXT(session->catalog_lookup.cache_memory_context);
		session->catalog_lookup.cache_memory_context = NULL;
	}

	PgSessionInitializePlanCacheState(&session->plan_cache);
}

static void
PgSessionResetNamespaceClosedState(PgSession *session)
{
	Assert(session != NULL);

	/*
	 * Normal namespace cleanup owns temp-namespace relation removal and GUC
	 * cleanup owns namespace_search_path_value.  Closed-session reset only
	 * releases the derived search-path storage/cache contexts and clears the
	 * remaining slots.
	 */
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->namespace_state.search_path_context);
	PG_RUNTIME_DELETE_MEMORY_CONTEXT(
		session->namespace_state.search_path_cache_context);
	session->namespace_state.active_search_path = NIL;
	session->namespace_state.active_creation_namespace = InvalidOid;
	session->namespace_state.active_temp_creation_pending = false;
	session->namespace_state.active_path_generation = 1;
	session->namespace_state.base_search_path = NIL;
	session->namespace_state.base_creation_namespace = InvalidOid;
	session->namespace_state.base_temp_creation_pending = false;
	session->namespace_state.namespace_user = InvalidOid;
	session->namespace_state.base_search_path_valid = true;
	session->namespace_state.search_path_cache_valid = false;
	session->namespace_state.search_path_context = NULL;
	session->namespace_state.search_path_cache_context = NULL;
	session->namespace_state.my_temp_namespace = InvalidOid;
	session->namespace_state.my_temp_toast_namespace = InvalidOid;
	session->namespace_state.my_temp_namespace_subid = InvalidSubTransactionId;
	session->namespace_state.namespace_search_path_value = NULL;
	session->namespace_state.search_path_cache = NULL;
	session->namespace_state.last_search_path_cache_entry = NULL;
	session->namespace_state.initialized = true;
}

void
PgSessionResetClosedState(PgSession *session)
{
	if (session == NULL)
		return;

	/*
	 * Bootstrap exits the process after proc_exit(); its adopted early session
	 * state is not a reusable backend session and may contain partially
	 * initialized callback/list state.
	 */
	if (IsBootstrapProcessingMode())
		return;

#define PG_SESSION_RESET_BUCKET(field, reset) \
	do { reset; } while (0);
#include "backend_runtime_session_reset_buckets.def"
#undef PG_SESSION_RESET_BUCKET
}

static void
PgExecutionResetDebugClosedState(PgExecution *execution)
{
	Assert(execution != NULL);

	execution->debug.debug_query_string = NULL;
}

static void
PgExecutionResetMemoryContextsClosedState(PgExecution *execution)
{
	bool		preserve_error_context;
	MemoryContext error_context;

	Assert(execution != NULL);

	/*
	 * Backend finish still has to log the final process/thread exit after
	 * closed-state reset.  Keep the backend's ErrorContext address usable for
	 * any ereport() on that final path, while clearing Top/CurrentMemoryContext
	 * so a threaded carrier can delete the retained root deliberately.
	 */
	preserve_error_context =
		PgBackendExitInProgress() &&
		execution == CurrentPgExecution;
	error_context = preserve_error_context ?
		execution->memory_contexts.error_context : NULL;

	PG_RUNTIME_DELETE_MEMORY_CONTEXT(execution->memory_contexts.message_context);

	MemSet(&execution->memory_contexts, 0,
		   sizeof(execution->memory_contexts));

	if (preserve_error_context)
		execution->memory_contexts.error_context = error_context;
}

static void
PgExecutionResetExtensionClosedState(PgExecutionExtensionState *extension)
{
	Assert(extension != NULL);

	foreach_ptr(PgExecutionExtensionPrivateState, private_state,
				extension->private_states)
	{
		if (private_state->cleanup != NULL &&
			private_state->state != NULL)
			private_state->cleanup(private_state->state);
	}

	foreach_ptr(PgExecutionExtensionPrivateState, private_state,
				extension->private_states)
	{
		if (private_state->state != NULL)
			pfree(private_state->state);
	}
	list_free_deep(extension->private_states);

	PgExecutionInitializeExtensionState(extension);
}

void
PgExecutionResetClosedState(PgExecution *execution)
{
	if (execution == NULL)
		return;

	if (execution == CurrentPgExecution)
		PgRuntimeFlushCurrentHotCells();
	if (execution == CurrentPgExecution)
		PgRuntimeFlushCurrentHotMirrors();

#define PG_EXECUTION_BUCKET(field, init, adopt, reset) \
	do { reset; } while (0);
#include "backend_runtime_execution_buckets.def"
#undef PG_EXECUTION_BUCKET

	if (execution == CurrentPgExecution)
		PgRuntimeReloadCurrentHotCells();
	if (execution == CurrentPgExecution)
		PgRuntimeReloadCurrentHotMirrors();
}
