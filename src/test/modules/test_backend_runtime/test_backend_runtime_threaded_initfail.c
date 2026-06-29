/*--------------------------------------------------------------------------
 *
 * test_backend_runtime_threaded_initfail.c
 *		Thread-compatible module that intentionally fails _PG_init().
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_backend_runtime/test_backend_runtime_threaded_initfail.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_backend_runtime_threaded_initfail",
					.version = PG_VERSION,
					PG_MODULE_MAGIC_BACKEND_MODEL_THREAD_PER_SESSION
);

PGDLLEXPORT void _PG_init(void);

void
_PG_init(void)
{
	ereport(ERROR,
			(errmsg("test_backend_runtime requested _PG_init failure")));
}
