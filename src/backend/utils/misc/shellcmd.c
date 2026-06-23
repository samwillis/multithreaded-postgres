/*-------------------------------------------------------------------------
 *
 * shellcmd.c
 *	  Server-side shell command execution.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/backend/utils/misc/shellcmd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifndef WIN32
#include <spawn.h>
#endif
#include <sys/wait.h>

#include "utils/backend_runtime.h"
#include "utils/shellcmd.h"

int
ExecuteShellCommand(const char *command)
{
#ifndef WIN32
	pid_t		pid;
	int			rc;
	int			status;
	posix_spawnattr_t attr;
	short		flags;
	sigset_t	empty;
	sigset_t	defaults;
	char	   *argv[4];
	extern char **environ;

	if (!PgRuntimeIsThreadBacked(CurrentPgRuntime))
		return system(command);

	/*
	 * system() changes SIGINT and SIGQUIT dispositions in the whole process
	 * while it waits.  In threaded mode that can make the postmaster miss a
	 * concurrent fast or immediate shutdown request, so run the shell command
	 * directly with child-local signal setup.
	 */
	rc = posix_spawnattr_init(&attr);
	if (rc != 0)
	{
		errno = rc;
		return -1;
	}

	sigemptyset(&empty);
	sigemptyset(&defaults);
	sigaddset(&defaults, SIGINT);
	sigaddset(&defaults, SIGQUIT);

	flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
	rc = posix_spawnattr_setsigdefault(&attr, &defaults);
	if (rc == 0)
		rc = posix_spawnattr_setsigmask(&attr, &empty);
	if (rc == 0)
		rc = posix_spawnattr_setflags(&attr, flags);
	if (rc != 0)
	{
		(void) posix_spawnattr_destroy(&attr);
		errno = rc;
		return -1;
	}

	argv[0] = "sh";
	argv[1] = "-c";
	argv[2] = unconstify(char *, command);
	argv[3] = NULL;

	rc = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
	(void) posix_spawnattr_destroy(&attr);
	if (rc != 0)
	{
		errno = rc;
		return -1;
	}

	do
	{
		rc = waitpid(pid, &status, 0);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		return -1;

	return status;
#else
	return system(command);
#endif
}
