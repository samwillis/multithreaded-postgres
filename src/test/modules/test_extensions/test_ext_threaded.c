/*
 * test_ext_threaded.c
 *
 * Minimal thread-per-session-safe module for extension backend-model tests.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 */
#include "postgres.h"

#include "fmgr.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_ext_threaded",
					.version = PG_VERSION,
					PG_MODULE_MAGIC_BACKEND_MODEL_THREAD_PER_SESSION
);

PG_FUNCTION_INFO_V1(test_ext);

Datum
test_ext(PG_FUNCTION_ARGS)
{
	ereport(NOTICE,
			(errmsg("running successful")));
	PG_RETURN_VOID();
}
