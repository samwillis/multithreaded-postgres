/*--------------------------------------------------------------------------
 *
 * test_backend_runtime_carrier.c
 *		Carrier-owned runtime state tests.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_backend_runtime/test_backend_runtime_carrier.c
 *
 * -------------------------------------------------------------------------
 */
#include "test_backend_runtime.h"

PG_FUNCTION_INFO_V1(test_carrier_misc_state_is_carrier_local);
Datum
test_carrier_misc_state_is_carrier_local(PG_FUNCTION_ARGS)
{
#define CHECK_CARRIER_MISC(expr) \
	do { \
		if (!(expr)) \
			elog(ERROR, "carrier miscellaneous state was not carrier-local: %s", \
				 #expr); \
	} while (0)

	PgCarrier  *saved_carrier;
	PgCarrier	fake_carrier1;
	PgCarrier	fake_carrier2;
	char		stack_marker1;
	char		stack_marker2;
	void	   *thread_start1 = &fake_carrier1;
	void	   *thread_start2 = &fake_carrier2;
	bool		saved_is_under_postmaster;

	saved_carrier = CurrentPgCarrier;
	saved_is_under_postmaster = IsUnderPostmaster;
	MemSet(&fake_carrier1, 0, sizeof(fake_carrier1));
	MemSet(&fake_carrier2, 0, sizeof(fake_carrier2));
	fake_carrier1.kind = PG_CARRIER_THREAD;
	fake_carrier2.kind = PG_CARRIER_THREAD;
	fake_carrier1.wait_event_signal_fd = -1;
	fake_carrier1.wait_event_selfpipe_readfd = -1;
	fake_carrier1.wait_event_selfpipe_writefd = -1;
	fake_carrier2.wait_event_signal_fd = -1;
	fake_carrier2.wait_event_selfpipe_readfd = -1;
	fake_carrier2.wait_event_selfpipe_writefd = -1;

	PG_TRY();
	{
		PgSetCurrentCarrier(&fake_carrier1);
		*PgCurrentWaitEventWaitingRef() = true;
		*PgCurrentWaitEventSignalFdRef() = 11;
		*PgCurrentWaitEventSelfPipeReadFdRef() = 12;
		*PgCurrentWaitEventSelfPipeWriteFdRef() = 13;
		*PgCurrentWaitEventSelfPipeOwnerPidRef() = 14;
		*PgCurrentStackBasePtrRef() = &stack_marker1;
		*PgCurrentBackendThreadStartRef() = thread_start1;
		IsUnderPostmaster = true;

		PgSetCurrentCarrier(&fake_carrier2);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventWaitingRef() == false);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSignalFdRef() == -1);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeReadFdRef() == -1);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeWriteFdRef() == -1);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeOwnerPidRef() == 0);
		CHECK_CARRIER_MISC(*PgCurrentStackBasePtrRef() == NULL);
		CHECK_CARRIER_MISC(*PgCurrentBackendThreadStartRef() == NULL);
		CHECK_CARRIER_MISC(!IsUnderPostmaster);
		*PgCurrentWaitEventWaitingRef() = false;
		*PgCurrentWaitEventSignalFdRef() = 21;
		*PgCurrentWaitEventSelfPipeReadFdRef() = 22;
		*PgCurrentWaitEventSelfPipeWriteFdRef() = 23;
		*PgCurrentWaitEventSelfPipeOwnerPidRef() = 24;
		*PgCurrentStackBasePtrRef() = &stack_marker2;
		*PgCurrentBackendThreadStartRef() = thread_start2;
		IsUnderPostmaster = false;

		PgSetCurrentCarrier(&fake_carrier1);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventWaitingRef() == true);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSignalFdRef() == 11);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeReadFdRef() == 12);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeWriteFdRef() == 13);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeOwnerPidRef() == 14);
		CHECK_CARRIER_MISC(*PgCurrentStackBasePtrRef() == &stack_marker1);
		CHECK_CARRIER_MISC(*PgCurrentBackendThreadStartRef() == thread_start1);
		CHECK_CARRIER_MISC(IsUnderPostmaster);

		PgSetCurrentCarrier(&fake_carrier2);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventWaitingRef() == false);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSignalFdRef() == 21);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeReadFdRef() == 22);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeWriteFdRef() == 23);
		CHECK_CARRIER_MISC(*PgCurrentWaitEventSelfPipeOwnerPidRef() == 24);
		CHECK_CARRIER_MISC(*PgCurrentStackBasePtrRef() == &stack_marker2);
		CHECK_CARRIER_MISC(*PgCurrentBackendThreadStartRef() == thread_start2);
		CHECK_CARRIER_MISC(!IsUnderPostmaster);

		PgSetCurrentCarrier(saved_carrier);
		IsUnderPostmaster = saved_is_under_postmaster;
	}
	PG_CATCH();
	{
		PgSetCurrentCarrier(saved_carrier);
		IsUnderPostmaster = saved_is_under_postmaster;
		PG_RE_THROW();
	}
	PG_END_TRY();

#undef CHECK_CARRIER_MISC
	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(test_carrier_threaded_guc_lock_depth_is_carrier_local);
Datum
test_carrier_threaded_guc_lock_depth_is_carrier_local(PG_FUNCTION_ARGS)
{
	PgCarrier  *saved_carrier;
	PgCarrier	fake_carrier1;
	PgCarrier	fake_carrier2;
	bool		ok = true;

	saved_carrier = CurrentPgCarrier;
	MemSet(&fake_carrier1, 0, sizeof(fake_carrier1));
	MemSet(&fake_carrier2, 0, sizeof(fake_carrier2));
	fake_carrier1.kind = PG_CARRIER_THREAD;
	fake_carrier2.kind = PG_CARRIER_THREAD;

	PG_TRY();
	{
		PgSetCurrentCarrier(&fake_carrier1);
		*PgCurrentThreadedGUCMutexDepthRef() = 1;
		PgSetCurrentCarrier(&fake_carrier2);
		ok = ok && *PgCurrentThreadedGUCMutexDepthRef() == 0;
		*PgCurrentThreadedGUCMutexDepthRef() = 2;
		PgSetCurrentCarrier(&fake_carrier1);
		ok = ok && *PgCurrentThreadedGUCMutexDepthRef() == 1;
		PgSetCurrentCarrier(&fake_carrier2);
		ok = ok && *PgCurrentThreadedGUCMutexDepthRef() == 2;

		PgSetCurrentCarrier(saved_carrier);
	}
	PG_CATCH();
	{
		PgSetCurrentCarrier(saved_carrier);
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (!ok)
		elog(ERROR, "threaded GUC mutex depth was not carrier-local");

	PG_RETURN_BOOL(true);
}
