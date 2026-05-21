#ifndef DENY_H
#define DENY_H

// Shared (kernel + user) definitions for the configurable command deny list.
// The deny list is the F7 hard sandbox boundary: command names on it never
// reach the agent runtime. It is mutated only by non-agent processes via the
// set_deny() system call (see kernel/sysproc.c) and managed from the shell by
// user/denyctl.c.

// Operations for set_deny(op, cmd).
#define DENY_ADD    0   // add cmd to the deny list (idempotent)
#define DENY_REMOVE 1   // remove cmd from the deny list
#define DENY_RESET  2   // restore the built-in default {KILL, EXEC}; cmd ignored
#define DENY_CLEAR  3   // empty the deny list; cmd ignored (used by `denyctl load`)

#define DENY_MAX     16   // max command names held in the deny list
#define DENY_NAMELEN 24   // max bytes per command name, including the NUL

#endif
