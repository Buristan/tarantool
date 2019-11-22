#ifndef TARANTOOL_LIB_CORE_POPEN_H_INCLUDED
#define TARANTOOL_LIB_CORE_POPEN_H_INCLUDED

#if defined(__cplusplus)
extern "C" {
#endif

#include <signal.h>
#include <sys/types.h>

#include <small/rlist.h>

#include "third_party/tarantool_ev.h"

enum popen_flag_bits {
	POPEN_FLAG_NONE			= (0 << 0),

	/*
	 * Which fd we should handle.
	 */
	POPEN_FLAG_FD_STDIN_BIT		= 0,
	POPEN_FLAG_FD_STDIN		= (1 << POPEN_FLAG_FD_STDIN_BIT),

	POPEN_FLAG_FD_STDOUT_BIT	= 1,
	POPEN_FLAG_FD_STDOUT		= (1 << POPEN_FLAG_FD_STDOUT_BIT),

	POPEN_FLAG_FD_STDERR_BIT	= 2,
	POPEN_FLAG_FD_STDERR		= (1 << POPEN_FLAG_FD_STDERR_BIT),

	/*
	 * Number of bits occupied for stdX descriptors.
	 */
	POPEN_FLAG_FD_STDEND_BIT	= POPEN_FLAG_FD_STDERR_BIT + 1,

	/*
	 * Instead of inheriting fds from a parent
	 * rather use /dev/null.
	 */
	POPEN_FLAG_FD_STDIN_DEVNULL_BIT	= 3,
	POPEN_FLAG_FD_STDIN_DEVNULL	= (1 << POPEN_FLAG_FD_STDIN_DEVNULL_BIT),
	POPEN_FLAG_FD_STDOUT_DEVNULL_BIT= 4,
	POPEN_FLAG_FD_STDOUT_DEVNULL	= (1 << POPEN_FLAG_FD_STDOUT_DEVNULL_BIT),
	POPEN_FLAG_FD_STDERR_DEVNULL_BIT= 5,
	POPEN_FLAG_FD_STDERR_DEVNULL	= (1 << POPEN_FLAG_FD_STDERR_DEVNULL_BIT),

	/*
	 * Instead of inheriting fds from a parent
	 * close fds completely.
	 */
	POPEN_FLAG_FD_STDIN_CLOSE_BIT	= 6,
	POPEN_FLAG_FD_STDIN_CLOSE	= (1 << POPEN_FLAG_FD_STDIN_CLOSE_BIT),
	POPEN_FLAG_FD_STDOUT_CLOSE_BIT	= 7,
	POPEN_FLAG_FD_STDOUT_CLOSE	= (1 << POPEN_FLAG_FD_STDOUT_CLOSE_BIT),
	POPEN_FLAG_FD_STDERR_CLOSE_BIT	= 8,
	POPEN_FLAG_FD_STDERR_CLOSE	= (1 << POPEN_FLAG_FD_STDERR_CLOSE_BIT),

	/*
	 * Call exec directly or via shell.
	 */
	POPEN_FLAG_SHELL_BIT		= 9,
	POPEN_FLAG_SHELL		= (1 << POPEN_FLAG_SHELL_BIT),
};

/**
 * struct popen_handle - an instance of popen object
 *
 * @pid:	pid of a child process
 * @wstatus:	exit status of a child process
 * @ev_sigchld:	needed by the libev to watch children
 * @flags:	popen_flag_bits
 * @fds:	std(in|out|err)
 */
struct popen_handle {
	pid_t			pid;
	int			wstatus;
	ev_child		ev_sigchld;
	struct rlist		list;
	unsigned int		flags;
	int			fds[POPEN_FLAG_FD_STDEND_BIT];
};

/**
 * struct popen_handle - popen object statistics
 *
 * @pid:	pid of a child process
 * @wstatus:	exit status of a child process
 * @flags:	popen_flag_bits
 * @fds:	std(in|out|err)
 *
 * This is a short version of struct popen_handle which should
 * be used by external code and which should be changed/extended
 * with extreme caution since it is used in Lua code. Consider it
 * as API for external modules.
 */
struct popen_stat {
	pid_t			pid;
	unsigned int		flags;
	int			fds[POPEN_FLAG_FD_STDEND_BIT];
};

extern int
popen_stat(struct popen_handle *handle, struct popen_stat *st);

extern ssize_t
popen_write(struct popen_handle *handle, void *buf,
	    size_t count, unsigned int flags);

extern ssize_t
popen_read_timeout(struct popen_handle *handle, void *buf,
		   size_t count, unsigned int flags,
		   int timeout_msecs);

extern int
popen_wstatus(struct popen_handle *handle, int *wstatus);

extern int
popen_kill(struct popen_handle *handle);

extern int
popen_destroy(struct popen_handle *handle);

extern struct popen_handle *
popen_create(const char *command, unsigned int flags);

extern void
popen_init(void);

extern void
popen_fini(void);

#if defined(__cplusplus)
} /* extern "C" */
#endif

#endif /* TARANTOOL_LIB_CORE_POPEN_H_INCLUDED */
