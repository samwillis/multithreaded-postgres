/*-------------------------------------------------------------------------
 *
 * shellcmd.h
 *	  Server-side shell command execution.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/utils/shellcmd.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SHELLCMD_H
#define SHELLCMD_H

extern int	ExecuteShellCommand(const char *command);

#endif							/* SHELLCMD_H */
