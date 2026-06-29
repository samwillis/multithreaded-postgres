/*-------------------------------------------------------------------------
 *
 * delay_execution.c
 *		Test module to allow delay between parsing and execution of a query.
 *
 * The delay is implemented by taking and immediately releasing a specified
 * advisory lock.  If another process has previously taken that lock, the
 * current process will be blocked until the lock is released; otherwise,
 * there's no effect.  This allows an isolationtester script to reliably
 * test behaviors where some specified action happens in another backend
 * between parsing and execution of any desired query.
 *
 * Copyright (c) 2020-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/delay_execution/delay_execution.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "optimizer/planner.h"
#include "utils/backend_runtime.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/inval.h"


PG_MODULE_MAGIC_EXT(
					.name = "delay_execution",
					.version = PG_VERSION,
					PG_MODULE_MAGIC_BACKEND_MODEL_THREAD_PER_SESSION
);

#define DELAY_EXECUTION_SESSION_STATE_KEY "delay_execution.session"
#define DELAY_EXECUTION_RUNTIME_STATE_KEY "delay_execution.runtime"

typedef struct DelayExecutionSessionState
{
	int			post_planning_lock_id;
} DelayExecutionSessionState;

typedef struct DelayExecutionRuntimeState
{
	planner_hook_type prev_planner_hook;
	bool		hook_installed;
} DelayExecutionRuntimeState;

static DelayExecutionSessionState *
delay_execution_session_state(void)
{
	return (DelayExecutionSessionState *)
		PgSessionEnsureExtensionPrivateState(DELAY_EXECUTION_SESSION_STATE_KEY,
											 sizeof(DelayExecutionSessionState),
											 NULL);
}

static DelayExecutionRuntimeState *
delay_execution_runtime_state(void)
{
	return (DelayExecutionRuntimeState *)
		PgRuntimeEnsureExtensionPrivateState(DELAY_EXECUTION_RUNTIME_STATE_KEY,
											 sizeof(DelayExecutionRuntimeState),
											 NULL);
}

/* GUC: advisory lock ID to use.  Zero disables the feature. */
#define post_planning_lock_id \
	(delay_execution_session_state()->post_planning_lock_id)

/* Save previous planner hook user to be a good citizen */
#define prev_planner_hook \
	(delay_execution_runtime_state()->prev_planner_hook)
#define delay_execution_hook_installed \
	(delay_execution_runtime_state()->hook_installed)


/* planner_hook function to provide the desired delay */
static PlannedStmt *
delay_execution_planner(Query *parse, const char *query_string,
						int cursorOptions, ParamListInfo boundParams,
						ExplainState *es)
{
	PlannedStmt *result;

	/* Invoke the planner, possibly via a previous hook user */
	if (prev_planner_hook)
		result = prev_planner_hook(parse, query_string, cursorOptions,
								   boundParams, es);
	else
		result = standard_planner(parse, query_string, cursorOptions,
								  boundParams, es);

	/* If enabled, delay by taking and releasing the specified lock */
	if (post_planning_lock_id != 0)
	{
		DirectFunctionCall1(pg_advisory_lock_int8,
							Int64GetDatum((int64) post_planning_lock_id));
		DirectFunctionCall1(pg_advisory_unlock_int8,
							Int64GetDatum((int64) post_planning_lock_id));

		/*
		 * Ensure that we notice any pending invalidations, since the advisory
		 * lock functions don't do this.
		 */
		AcceptInvalidationMessages();
	}

	return result;
}

/* Module load function */
void
_PG_init(void)
{
	/* Set up the GUC to control which lock is used */
	DefineCustomIntVariable("delay_execution.post_planning_lock_id",
							"Sets the advisory lock ID to be locked/unlocked after planning.",
							"Zero disables the delay.",
							&post_planning_lock_id,
							0,
							0, INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	MarkGUCPrefixReserved("delay_execution");

	/* Install our hook */
	if (!delay_execution_hook_installed)
	{
		prev_planner_hook = planner_hook;
		planner_hook = delay_execution_planner;
		delay_execution_hook_installed = true;
	}
}
