#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <paths.h>
#include <signal.h>
#include <poll.h>

#include <sys/types.h>
#include <dirent.h>
#include <sys/wait.h>

#include "popen.h"
#include "fiber.h"
#include "assoc.h"
#include "say.h"

/* Children pids map popen_handle map */
static struct mh_i32ptr_t *popen_pids_map = NULL;

/* All popen handles to be able to cleanup them on exit */
static RLIST_HEAD(popen_head);

/* /dev/null to be used inside children if requested */
static int dev_null_fd_ro = -1;
static int dev_null_fd_wr = -1;

/**
 * popen_register - register popen handle in a pids map
 * @handle:	handle to register
 */
static void
popen_register(struct popen_handle *handle)
{
	struct mh_i32ptr_node_t node = {
		.key	= handle->pid,
		.val	= handle,
	};
	say_debug("popen: register %d", handle->pid);
	mh_i32ptr_put(popen_pids_map, &node, NULL, NULL);
}

/**
 * popen_find - find popen handler by its pid
 * @pid:	pid of a handler
 *
 * Returns a handle if found or NULL otherwise.
 */
static struct popen_handle *
popen_find(pid_t pid)
{
	mh_int_t k = mh_i32ptr_find(popen_pids_map, pid, NULL);
	if (k == mh_end(popen_pids_map))
		return NULL;
	return mh_i32ptr_node(popen_pids_map, k)->val;
}

/**
 * popen_unregister - remove popen handler from a pids map
 * @handle:	handle to remove
 */
static void
popen_unregister(struct popen_handle *handle)
{
	struct mh_i32ptr_node_t node = {
		.key	= handle->pid,
		.val	= NULL,
	};
	say_debug("popen: unregister %d", handle->pid);
	mh_i32ptr_remove(popen_pids_map, &node, NULL);
}

/**
 * command_new - allocates a command string from argv array
 * @argv:	an array with pointers to argv strings
 * @nr_argv:	number of elements in the @argv
 *
 * Returns a new string or NULL on error.
 */
static inline char *
command_new(char **argv, size_t nr_argv)
{
	size_t size = 0, i;
	char *command;
	char *pos;

	assert(argv != NULL && nr_argv > 0);

	for (i = 0; i < nr_argv; i++) {
		if (argv[i] == NULL)
			continue;
		size += strlen(argv[i]) + 1;
	}

	command = malloc(size);
	if (!command) {
		say_syserror("popen: Can't allocate command");
		return NULL;
	}

	pos = command;
	for (i = 0; i < nr_argv-1; i++) {
		if (argv[i] == NULL)
			continue;
		strcpy(pos, argv[i]);
		pos += strlen(argv[i]);
		*pos++ = ' ';
	}
	pos[-1] = '\0';

	return command;
}

/**
 * command_free - free memory allocated for command
 * @command:	pointer to free
 *
 * To pair with command_new().
 */
static inline void
command_free(char *command)
{
	free(command);
}

/**
 * handle_new - allocate new popen hanldle with flags specified
 * @opts:	pointer to options to be used
 * @command:	command line string
 *
 * Returns pointer to a new initialized popen object
 * or NULL on error.
 */
static struct popen_handle *
handle_new(struct popen_opts *opts, char *command)
{
	struct popen_handle *handle;

	handle = malloc(sizeof(*handle));
	if (!handle) {
		say_syserror("popen: Can't allocate handle");
		return NULL;
	}

	handle->command	= command;
	handle->wstatus	= 0;
	handle->pid	= -1;
	handle->flags	= opts->flags;

	rlist_create(&handle->list);

	/* all fds to -1 */
	memset(handle->fds, 0xff, sizeof(handle->fds));

	say_debug("popen: handle %p alloc", handle);
	return handle;
}

/**
 * handle_free - free memory allocated for a handle
 * @handle:	popen handle to free
 *
 * Just to match handle_new().
 */
static void
handle_free(struct popen_handle *handle)
{
	say_debug("popen: handle %p free %p", handle);
	free(handle);
}

/**
 * popen_may_io - test if handle can run io operation
 * @handle:	popen handle
 * @idx:	index of a file descriptor to operate on
 * @io_flags:	popen_flag_bits flags
 *
 * Returns true if IO is allowed and false otherwise
 * (setting @errno).
 */
static inline bool
popen_may_io(struct popen_handle *handle, unsigned int idx,
	     unsigned int io_flags)
{
	if (!handle) {
		errno = ESRCH;
		return false;
	}

	if (!(io_flags & handle->flags)) {
		errno = EINVAL;
		return false;
	}

	if (handle->fds[idx] < 0) {
		errno = EPIPE;
		return false;
	}

	return true;
}

/**
 * popen_may_pidop - test if handle is valid for pid related operations
 * @handle:	popen handle
 *
 * This is shortcut to test if handle is not nil and still have
 * a valid child process.
 *
 * Returns true if ops are allowed and false otherwise
 * (setting @errno).
 */
static inline bool
popen_may_pidop(struct popen_handle *handle)
{
	if (!handle || handle->pid == -1) {
		errno = ESRCH;
		return false;
	}
	return true;
}

/**
 * popen_stat - fill popen object statistics
 * @handle:	popen handle
 * @st:		destination popen_stat to fill
 *
 * Returns 0 on succes, -1 otherwise (setting @errno).
 */
int
popen_stat(struct popen_handle *handle, struct popen_stat *st)
{
	if (!handle) {
		errno = ESRCH;
		return -1;
	}

	st->pid		= handle->pid;
	st->flags	= handle->flags;

	static_assert(lengthof(st->fds) == lengthof(handle->fds),
		      "Statistics fds are screwed");

	memcpy(st->fds, handle->fds, sizeof(st->fds));
	return 0;
}

/**
 * popen_command - get a pointer to former command line
 * @handle:	popen handle
 *
 * Returns pointer to a former command line for executiion.
 * Since it might be big enough it is moved out of popen_stat.
 *
 * On error NULL returned.
 */
const char *
popen_command(struct popen_handle *handle)
{
	if (!handle) {
		errno = ESRCH;
		return NULL;
	}

	return (const char *)handle->command;
}

/**
 * stdX_str - get stdX descriptor string representation
 * @index:	descriptor index
 */
static inline const char *
stdX_str(unsigned int index)
{
	static const char * stdX_names[] = {
		[STDIN_FILENO]	= "stdin",
		[STDOUT_FILENO]	= "stdout",
		[STDERR_FILENO]	= "stderr",
	};

	return index < lengthof(stdX_names) ?
		stdX_names[index] : "unknown";
}

/**
 * popen_write - write data to the child stdin
 * @handle:	popen handle
 * @buf:	data to write
 * @count:	number of bytes to write
 * @flags:	a flag representing stdin peer
 *
 * Returns number of bytes written or -1 on error (setting @errno).
 */
ssize_t
popen_write(struct popen_handle *handle, void *buf,
	    size_t count, unsigned int flags)
{
	if (!popen_may_io(handle, STDIN_FILENO, flags))
		return -1;

	if (count > (size_t)SSIZE_MAX) {
		errno = E2BIG;
		return -1;
	}

	say_debug("popen: %d: write idx [%s:%d] buf %p count %zu",
		  handle->pid, stdX_str(STDIN_FILENO),
		  STDIN_FILENO, buf, count);

	return write(handle->fds[STDIN_FILENO], buf, count);
}

/**
 * popen_wait_read - wait for data appear on a child's peer
 * @handle:		popen handle
 * @idx:		peer index to wait on
 * @timeout_msecs:	timeout in microseconds
 *
 * Returns 1 if there is data to read, -1 on error (setting @errno).
 */
static int
popen_wait_read(struct popen_handle *handle, int idx, int timeout_msecs)
{
	struct pollfd pollfd = {
		.fd	= handle->fds[idx],
		.events	= POLLIN,
	};
	int ret;

	ret = poll(&pollfd, 1, timeout_msecs);
	say_debug("popen: %d: poll: ret %d fd [%s:%d] events %#x revents %#x",
		  handle->pid, ret, stdX_str(idx), handle->fds[idx],
		  pollfd.events, pollfd.revents);

	if (ret == 1) {
		if (pollfd.revents == POLLIN) {
			return 1;
		} else {
			say_error("popen: %d: unexpected revents %#x",
				  handle->pid, pollfd.revents);
			errno = EINVAL;
			return -1;
		}
	} else if (ret == 0) {
		errno = EAGAIN;
		ret = -1;
	}

	return ret;
}

/**
 * popen_read_timeout - read data from a child's peer with timeout
 * @handle:		popen handle
 * @buf:		destination buffer
 * @count:		number of bytes to read
 * @flags:		POPEN_FLAG_FD_STDOUT or POPEN_FLAG_FD_STDERR
 * @timeout_msecs:	time to wait in microseconds if no
 * 			data available; ignored if less or equal to zero
 *
 * Returns number of bytes read, otherwise -1 returned and
 * @errno set accordingly.
 */
ssize_t
popen_read_timeout(struct popen_handle *handle, void *buf,
		   size_t count, unsigned int flags,
		   int timeout_msecs)
{
	ssize_t ret;
	int idx;

	idx = flags & POPEN_FLAG_FD_STDOUT ?
		STDOUT_FILENO : STDERR_FILENO;

	if (!popen_may_io(handle, idx, flags))
		return -1;

	if (count > (size_t)SSIZE_MAX) {
		errno = E2BIG;
		return -1;
	}

	say_debug("popen: %d: read idx [%s:%d] buf %p count %zu "
		  "fds %d timeout_msecs %d",
		  handle->pid, stdX_str(idx), idx, buf, count,
		  handle->fds[idx], timeout_msecs);

	if (timeout_msecs > 0) {
		ret = popen_wait_read(handle, idx, timeout_msecs);
		if (ret < 0) {
			if (errno != EAGAIN) {
				say_syserror("popen: %d: data wait failed",
					     handle->pid);
			}
			return -1;
		}
	}

	return read(handle->fds[idx], buf, count);
}

/**
 * wstatus_str - encode signal status into human readable form
 * @buf:	destination buffer
 * @size:	buffer size
 * @wstatus:	status to encode
 *
 * Operates on S_DEBUG level only simply because snprintf
 * is pretty heavy in performance, otherwise @buf remains
 * untouched.
 *
 * Returns pointer to @buf with encoded string.
 */
static char *
wstatus_str(char *buf, size_t size, int wstatus)
{
	static const char fmt[] =
		"wstatus %#x exited %d status %d "
		"signaled %d wtermsig %d "
		"stopped %d stopsig %d "
		"coredump %d continued %d";

	assert(size > 0);

	if (say_log_level_is_enabled(S_DEBUG)) {
		snprintf(buf, size, fmt, wstatus,
			 WIFEXITED(wstatus),
			 WIFEXITED(wstatus) ?
			 WEXITSTATUS(wstatus) : -1,
			 WIFSIGNALED(wstatus),
			 WIFSIGNALED(wstatus) ?
			 WTERMSIG(wstatus) : -1,
			 WIFSTOPPED(wstatus),
			 WIFSTOPPED(wstatus) ?
			 WSTOPSIG(wstatus) : -1,
			 WCOREDUMP(wstatus),
			 WIFCONTINUED(wstatus));
	}

	return buf;
}

/**
 * popen_notify_sigchld - notify popen subsistem about SIGCHLD event
 * @pid:	PID of a process which changed its state
 * @wstatus:	signal status of a process
 *
 * The function is called from global SIGCHLD watcher in libev so
 * we need to figure out if it is our process which possibly been
 * terminated.
 *
 * Note the libev calls for wait() by self so we don't need
 * to reap children.
 */
static void
popen_notify_sigchld(pid_t pid, int wstatus)
{
	struct popen_handle *handle;
	char buf[128];

	say_debug("popen: sigchld notify %d (%s)",
		  pid, wstatus_str(buf, sizeof(buf), wstatus));

	handle = popen_find(pid);
	if (!handle)
		return;

	handle->wstatus = wstatus;
	if (WIFEXITED(wstatus) || WIFSIGNALED(wstatus)) {
		assert(handle->pid == pid);
		/*
		 * libev calls for waitpid by self so
		 * we don't have to wait here.
		 */
		popen_unregister(handle);
		/*
		 * Since SIGCHLD may come to us not
		 * due to exit/kill reason (consider
		 * a case when someone stopped a child
		 * process) we should continue wathcing
		 * state changes, thus we stop monitoring
		 * dead children only.
		 */
		say_debug("popen: ev_child_stop %d", handle->pid);
		ev_child_stop(EV_DEFAULT_ &handle->ev_sigchld);
		handle->pid = -1;
	}
}

/**
 * ev_sigchld_cb - handle SIGCHLD from a child process
 * @w:		a child exited
 * @revents:	unused
 */
static void
ev_sigchld_cb(EV_P_ ev_child *w, int revents)
{
	(void)revents;
	/*
	 * Stop watching this child, libev will
	 * remove it from own hashtable.
	 */
	ev_child_stop(EV_A_ w);

	/*
	 * The reason for a separate helper is that
	 * we might need to notify more subsystems
	 * in future.
	 */
	popen_notify_sigchld(w->rpid, w->rstatus);
}

/**
 * popen_state - get current child state
 * @handle:	popen handle
 * @state:	state of a child process
 * @exit_code:	exit code of a child process
 *
 * On success returns 0 sets @state to  POPEN_STATE_
 * constant and @exit_code. On error -1 returned with @errno.
 */
int
popen_state(struct popen_handle *handle, int *state, int *exit_code)
{
	if (!handle) {
		errno = ESRCH;
		return -1;
	}

	if (handle->pid != -1) {
		*state = POPEN_STATE_ALIVE;
		*exit_code = 0;
	} else {
		if (WIFEXITED(handle->wstatus)) {
			*state = POPEN_STATE_EXITED;
			*exit_code = WEXITSTATUS(handle->wstatus);
		} else {
			*state = POPEN_STATE_SIGNALED;
			*exit_code = WTERMSIG(handle->wstatus);
		}
	}

	return 0;
}

/**
 * popen_state_str - get process state string representation
 * @state:	process state
 */
const char *
popen_state_str(unsigned int state)
{
	/*
	 * A process may be in a number of states,
	 * running/sleeping/disk sleep/stopped and etc
	 * but we are interested only if it is alive
	 * or dead (via plain exit or kill signal).
	 *
	 * Thus while it present in a system and not
	 * yet reaped we call it "alive".
	 *
	 * Note this is API for lua, so change with
	 * caution if needed.
	 */
	static const char * state_str[POPEN_STATE_MAX] = {
		[POPEN_STATE_NONE]	= "none",
		[POPEN_STATE_ALIVE]	= "alive",
		[POPEN_STATE_EXITED]	= "exited",
		[POPEN_STATE_SIGNALED]	= "signaled",
	};

	return state < POPEN_STATE_MAX ? state_str[state] : "unknown";
}

/**
 * popen_send_signal - send a signal to a child process
 * @handle:	popen handle
 * @signo:	signal number
 *
 * Returns 0 on success, -1 otherwise.
 */
int
popen_send_signal(struct popen_handle *handle, int signo)
{
	int ret;

	/*
	 * A child may be killed or exited already.
	 */
	if (!popen_may_pidop(handle))
		return -1;

	if (handle->flags & POPEN_FLAGS_SETSID) {
		say_debug("popen: killpg %d signo %d", handle->pid, signo);
		ret = killpg(handle->pid, signo);
	} else {
		say_debug("popen: kill %d signo %d", handle->pid, signo);
		ret = kill(handle->pid, signo);
	}
	if (ret < 0) {
		say_syserror("popen: Unable to killpg %d signo %d",
			     handle->pid, signo);
	}
	return ret;
}

/**
 * popen_delete - delete a popen handle
 * @handle:	a popen handle to delete
 *
 * The function kills a child process and
 * close all fds and remove the handle from
 * a living list and finally frees the handle.
 *
 * Returns 0 on success, -1 otherwise.
 */
int
popen_delete(struct popen_handle *handle)
{
	size_t i;

	if (!handle) {
		errno = ESRCH;
		return -1;
	}

	if (popen_send_signal(handle, SIGKILL) && errno != ESRCH)
		return -1;

	for (i = 0; i < lengthof(handle->fds); i++) {
		if (handle->fds[i] != -1)
			close(handle->fds[i]);
	}

	/*
	 * We won't be wathcing this child anymore if
	 * kill signal is not yet delivered.
	 */
	if (handle->pid != -1) {
		say_debug("popen: ev_child_stop %d", handle->pid);
		ev_child_stop(EV_DEFAULT_ &handle->ev_sigchld);
	}

	rlist_del(&handle->list);
	command_free(handle->command);
	handle_free(handle);
	return 0;
}

/**
 * make_pipe - create O_CLOEXEC pipe
 * @pfd:	pipe ends to setup
 *
 * Returns 0 on success, -1 on error.
 */
static int
make_pipe(int pfd[2])
{
#ifdef TARGET_OS_LINUX
	if (pipe2(pfd, O_CLOEXEC)) {
		say_syserror("popen: Can't create pipe2");
		return -1;
	}
#else
	if (pipe(pfd)) {
		say_syserror("popen: Can't create pipe");
		return -1;
	}
	if (fcntl(pfd[0], F_SETFL, O_CLOEXEC) ||
	    fcntl(pfd[1], F_SETFL, O_CLOEXEC)) {
		int saved_errno = errno;
		say_syserror("popen: Can't unblock pipe");
		close(pfd[0]), pfd[0] = -1;
		close(pfd[1]), pfd[1] = -1;
		errno = saved_errno;
		return -1;
	}
#endif
	return 0;
}

/**
 * close_inherited_fds - close inherited file descriptors
 * @skip_fds:		an array of descriptors which should
 * 			be kept opened
 * @nr_skip_fds:	number of elements in @skip_fds
 *
 * Returns 0 on success, -1 otherwise.
 */
static int
close_inherited_fds(int *skip_fds, size_t nr_skip_fds)
{
#ifdef TARGET_OS_LINUX
	static const char path[] = "/proc/self/fd";
	struct dirent *de;
	int fd_no, fd_dir;
	DIR *dir;
	size_t i;

	dir = opendir(path);
	if (!dir) {
		say_syserror("popen: fdin: Can't open %s", path);
		return -1;
	}
	fd_dir = dirfd(dir);

	for (de = readdir(dir); de; de = readdir(dir)) {
		if (!strcmp(de->d_name, ".") ||
		    !strcmp(de->d_name, ".."))
			continue;

		fd_no = atoi(de->d_name);

		if (fd_no == fd_dir)
			continue;

		/* We don't expect many numbers here */
		for (i = 0; i < nr_skip_fds; i++) {
			if (fd_no == skip_fds[i]) {
				fd_no = -1;
				break;
			}
		}

		if (fd_no == -1)
			continue;

		if (close(fd_no)) {
			int saved_errno = errno;
			say_syserror("popen: fdin: Can't close %d", fd_no);
			closedir(dir);
			errno = saved_errno;
			return -1;
		}
	}

	if (closedir(dir)) {
		say_syserror("popen: fdin: Can't close %s", path);
		return -1;
	}
#else
	/* FIXME: What about FreeBSD/MachO? */
	(void)skip_fds;
	(void)nr_skip_fds;

	static bool said = false;
	if (!said) {
		say_warn("popen: fdin: Skip closing inherited");
		said = true;
	}
#endif
	return 0;
}

#ifdef TARGET_OS_LINUX
extern char **environ;
#endif

static inline char **
get_envp(struct popen_opts *opts)
{
	if (!opts->env) {
#ifdef TARGET_OS_LINUX
		return environ;
#else
		static const char **empty_envp[] = { NULL };
		static bool said = false;
		if (!said)
			say_warn("popen: Environment inheritance "
				 "unsupported, passing empty");
		return (char *)empty_envp;
#endif
	}
	return opts->env;
}

/**
 * signal_reset - reset signals to default before executing a program
 *
 * FIXME: This is a code duplication fomr main.cc. Need to rework
 * signal handing otherwise it will become utter crap very fast.
 */
static void
signal_reset(void)
{
	struct sigaction sa = { };
	sigset_t sigset;

	/* Reset all signals to their defaults. */
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_DFL;

	if (sigaction(SIGUSR1, &sa, NULL) == -1 ||
	    sigaction(SIGINT, &sa, NULL) == -1 ||
	    sigaction(SIGTERM, &sa, NULL) == -1 ||
	    sigaction(SIGHUP, &sa, NULL) == -1 ||
	    sigaction(SIGWINCH, &sa, NULL) == -1 ||
	    sigaction(SIGSEGV, &sa, NULL) == -1 ||
	    sigaction(SIGFPE, &sa, NULL) == -1)
		_exit(errno);

	/* Unblock any signals blocked by libev */
	sigfillset(&sigset);
	if (sigprocmask(SIG_UNBLOCK, &sigset, NULL) == -1)
		_exit(errno);
}

/**
 * popen_new - Create new popen handle
 * @opts: options for popen handle
 *
 * This function creates a new child process and passes it
 * pipe ends to communicate with child's stdin/stdout/stderr
 * depending on @opts:flags. Where @opts:flags could be
 * the bitwise "OR" for the following values:
 *
 *  POPEN_FLAG_FD_STDIN		- to write to stdin
 *  POPEN_FLAG_FD_STDOUT	- to read from stdout
 *  POPEN_FLAG_FD_STDERR	- to read from stderr
 *
 * When need to pass /dev/null descriptor into a child
 * the following values can be used:
 *
 *  POPEN_FLAG_FD_STDIN_DEVNULL
 *  POPEN_FLAG_FD_STDOUT_DEVNULL
 *  POPEN_FLAG_FD_STDERR_DEVNULL
 *
 * These flags have no effect if appropriate POPEN_FLAG_FD_STDx
 * flags are set.
 *
 * When need to completely close the descriptors the
 * following values can be used:
 *
 *  POPEN_FLAG_FD_STDIN_CLOSE
 *  POPEN_FLAG_FD_STDOUT_CLOSE
 *  POPEN_FLAG_FD_STDERR_CLOSE
 *
 * These flags have no effect if appropriate POPEN_FLAG_FD_STDx
 * flags are set.
 *
 * If none of POPEN_FLAG_FD_STDx flags are specified the child
 * process will run with all files inherited from a parent.
 *
 * Returns pointer to a new popen handle on success,
 * otherwise NULL returned setting @errno.
 */
struct popen_handle *
popen_new(struct popen_opts *opts)
{
	struct popen_handle *handle = NULL;
	char *command = NULL;

	int pfd[POPEN_FLAG_FD_STDEND_BIT][2] = {
		{-1, -1}, {-1, -1}, {-1, -1},
	};

	char **envp = get_envp(opts);
	int saved_errno;
	size_t i;

	static const struct {
		unsigned int	mask;
		unsigned int	mask_devnull;
		unsigned int	mask_close;
		int		fileno;
		int		*dev_null_fd;
		int		parent_idx;
		int		child_idx;
		bool		nonblock;
	} pfd_map[POPEN_FLAG_FD_STDEND_BIT] = {
		{
			.mask		= POPEN_FLAG_FD_STDIN,
			.mask_devnull	= POPEN_FLAG_FD_STDIN_DEVNULL,
			.mask_close	= POPEN_FLAG_FD_STDIN_CLOSE,
			.fileno		= STDIN_FILENO,
			.dev_null_fd	= &dev_null_fd_ro,
			.parent_idx	= 1,
			.child_idx	= 0,
		}, {
			.mask		= POPEN_FLAG_FD_STDOUT,
			.mask_devnull	= POPEN_FLAG_FD_STDOUT_DEVNULL,
			.mask_close	= POPEN_FLAG_FD_STDOUT_CLOSE,
			.fileno		= STDOUT_FILENO,
			.dev_null_fd	= &dev_null_fd_wr,
			.parent_idx	= 0,
			.child_idx	= 1,
		}, {
			.mask		= POPEN_FLAG_FD_STDERR,
			.mask_devnull	= POPEN_FLAG_FD_STDERR_DEVNULL,
			.mask_close	= POPEN_FLAG_FD_STDERR_CLOSE,
			.fileno		= STDERR_FILENO,
			.dev_null_fd	= &dev_null_fd_wr,
			.parent_idx	= 0,
			.child_idx	= 1,
		},
	};
	/*
	 * At max we could be skipping each pipe end
	 * plus dev/null variants.
	 */
	int skip_fds[POPEN_FLAG_FD_STDEND_BIT * 2 + 2];
	size_t nr_skip_fds = 0;

	/*
	 * A caller must preserve space for this.
	 */
	if (opts->flags & POPEN_FLAG_SHELL) {
		opts->argv[0] = "sh";
		opts->argv[1] = "-c";
	}

	/*
	 * Carry a copy for information sake.
	 */
	command = command_new(opts->argv, opts->nr_argv);

	say_debug("popen: command '%s' flags %#x", command, opts->flags);

	static_assert(STDIN_FILENO == 0 &&
		      STDOUT_FILENO == 1 &&
		      STDERR_FILENO == 2,
		      "stdin/out/err are not posix compatible");

	static_assert(lengthof(pfd) == lengthof(pfd_map),
		      "Pipes number does not map to fd bits");

	static_assert(POPEN_FLAG_FD_STDIN_BIT == STDIN_FILENO &&
		      POPEN_FLAG_FD_STDOUT_BIT == STDOUT_FILENO &&
		      POPEN_FLAG_FD_STDERR_BIT == STDERR_FILENO,
		      "Popen flags do not match stdX");

	handle = handle_new(opts, command);
	if (!handle)
		return NULL;

	skip_fds[nr_skip_fds++] = dev_null_fd_ro;
	skip_fds[nr_skip_fds++] = dev_null_fd_wr;
	assert(nr_skip_fds <= lengthof(skip_fds));

	for (i = 0; i < lengthof(pfd_map); i++) {
		if (opts->flags & pfd_map[i].mask) {
			if (make_pipe(pfd[i]))
				goto out_err;

			/*
			 * FIXME: Rather force make_pipe
			 * to allocate new fds with higher
			 * number.
			 */
			if (pfd[i][0] <= STDERR_FILENO ||
			    pfd[i][1] <= STDERR_FILENO) {
				say_error("popen: low fds [%s:%d:%d]",
					  stdX_str(i), pfd[i][0],
					  pfd[i][1]);
				errno = EBADF;
				goto out_err;
			}

			skip_fds[nr_skip_fds++] = pfd[i][0];
			skip_fds[nr_skip_fds++] = pfd[i][1];
			assert(nr_skip_fds <= lengthof(skip_fds));

			say_debug("popen: created pipe [%s:%d:%d]",
				  stdX_str(i), pfd[i][0], pfd[i][1]);
		} else if (!(opts->flags & pfd_map[i].mask_devnull) &&
			   !(opts->flags & pfd_map[i].mask_close)) {
			skip_fds[nr_skip_fds++] = pfd_map[i].fileno;

			say_debug("popen: inherit [%s:%d]",
				  stdX_str(i), pfd_map[i].fileno);
		}
	}

	/*
	 * We have to use vfork here because libev has own
	 * at_fork helpers with mutex, so we will have double
	 * lock here and stuck forever otherwise.
	 *
	 * The good news that this affect tx only the
	 * other tarantoll threads are not waiting for
	 * vfork to complete. Also we try to do as minimum
	 * operations before the exec() as possible.
	 */
	handle->pid = vfork();
	if (handle->pid < 0) {
		goto out_err;
	} else if (handle->pid == 0) {
		/*
		 * The documentation for libev says that
		 * each new fork should call ev_loop_fork(EV_DEFAULT)
		 * on every new child process, but we're going
		 * to execute bew binary anyway thus everything
		 * related to OS resources will be eliminated except
		 * file descriptors we use for piping. Thus don't
		 * do anything special.
		 */

		/*
		 * Also don't forget to drop signal handlers
		 * to default inside a child process since we're
		 * inheriting them from a caller process.
		 */
		if (opts->flags & POPEN_FLAGS_RESTORE_SIGNALS)
			signal_reset();

		/*
		 * We have to be a session leader otherwise
		 * won't be able to kill a group of children.
		 */
		if (opts->flags & POPEN_FLAGS_SETSID) {
			if (setsid() == -1)
				goto exit_child;
		}

		if (opts->flags & POPEN_FLAG_CLOSE_FDS) {
			if (close_inherited_fds(skip_fds, nr_skip_fds))
				goto exit_child;
		}

		for (i = 0; i < lengthof(pfd_map); i++) {
			int fileno = pfd_map[i].fileno;
			/*
			 * Pass pipe peer to a child.
			 */
			if (opts->flags & pfd_map[i].mask) {
				int child_idx = pfd_map[i].child_idx;

				/* put child peer end at known place */
				if (dup2(pfd[i][child_idx], fileno) < 0)
					goto exit_child;

				/* parent's pipe no longer needed */
				if (close(pfd[i][0]) ||
				    close(pfd[i][1]))
					goto exit_child;
				continue;
			}

			/*
			 * Use /dev/null if requested.
			 */
			if (opts->flags & pfd_map[i].mask_devnull) {
				if (dup2(*pfd_map[i].dev_null_fd,
					 fileno) < 0) {
					goto exit_child;
				}
				continue;
			}

			/*
			 * Or close the destination completely, since
			 * we don't if the file in question is already
			 * closed by some other code we don't care if
			 * EBADF happens.
			 */
			if (opts->flags & pfd_map[i].mask_close) {
				if (close(fileno) && errno != EBADF)
					goto exit_child;
				continue;
			}

			/*
			 * Otherwise inherit file descriptor
			 * from a parent.
			 */
		}

		if (close(dev_null_fd_ro) || close(dev_null_fd_wr))
			goto exit_child;

		if (opts->flags & POPEN_FLAG_SHELL)
			execve(_PATH_BSHELL, opts->argv, envp);
		else
			execve(opts->argv[2], &opts->argv[2], envp);
exit_child:
		_exit(errno);
		unreachable();
	}

	for (i = 0; i < lengthof(pfd_map); i++) {
		if (opts->flags & pfd_map[i].mask) {
			int parent_idx = pfd_map[i].parent_idx;
			int child_idx = pfd_map[i].child_idx;

			handle->fds[i] = pfd[i][parent_idx];
			say_debug("popen: keep pipe [%s:%d]",
				  stdX_str(i), handle->fds[i]);

			if (close(pfd[i][child_idx])) {
				say_syserror("popen: close child [%s:%d]",
					     stdX_str(i),
					     pfd[i][child_idx]);
				goto out_err;
			}

			pfd[i][child_idx] = -1;
		}
	}

	/*
	 * Link it into global list for force
	 * cleanup on exit.
	 */
	rlist_add(&popen_head, &handle->list);

	/*
	 * To watch when a child get exited.
	 */
	popen_register(handle);

	say_debug("popen: ev_child_start %d", handle->pid);
	ev_child_init(&handle->ev_sigchld, ev_sigchld_cb, handle->pid, 0);
	ev_child_start(EV_DEFAULT_ &handle->ev_sigchld);

	say_debug("popen: created child %d", handle->pid);

	return handle;

out_err:
	saved_errno = errno;
	popen_delete(handle);
	for (i = 0; i < lengthof(pfd); i++) {
		if (pfd[i][0] != -1)
			close(pfd[i][0]);
		if (pfd[i][1] != -1)
			close(pfd[i][1]);
	}
	errno = saved_errno;
	return NULL;
}

/**
 * popen_init - initialize popen subsystem
 *
 * Allocates resource needed for popen management.
 */
void
popen_init(void)
{
	static const int flags = O_CLOEXEC;
	static const char dev_null_path[] = "/dev/null";

	say_debug("popen: initialize subsystem");
	popen_pids_map = mh_i32ptr_new();

	dev_null_fd_ro = open(dev_null_path, O_RDONLY | flags);
	if (dev_null_fd_ro < 0)
		goto out_err;
	dev_null_fd_wr = open(dev_null_path, O_WRONLY | flags);
	if (dev_null_fd_wr < 0)
		goto out_err;

	/*
	 * FIXME: We should allocate them somewhere
	 * after STDERR_FILENO so the child would be
	 * able to find these fd numbers not occupied.
	 * Other option is to use unix scm and send
	 * them to the child on demand.
	 *
	 * For now left as is since we don't close
	 * our main stdX descriptors and they are
	 * inherited when we call first vfork.
	 */
	if (dev_null_fd_ro <= STDERR_FILENO ||
	    dev_null_fd_wr <= STDERR_FILENO) {
		say_error("popen: /dev/null %d %d numbers are too low",
			  dev_null_fd_ro, dev_null_fd_wr);
		goto out_err;
	}

	return;

out_err:
	say_syserror("popen: Can't open %s", dev_null_path);
	if (dev_null_fd_ro >= 0)
		close(dev_null_fd_ro);
	if (dev_null_fd_wr >= 0)
		close(dev_null_fd_wr);
	mh_i32ptr_delete(popen_pids_map);
	exit(EXIT_FAILURE);
}

/**
 * popen_free - free popen subsystem
 *
 * Kills all running children and frees resources.
 */
void
popen_free(void)
{
	struct popen_handle *handle, *tmp;

	say_debug("popen: free subsystem");

	close(dev_null_fd_ro);
	close(dev_null_fd_wr);
	dev_null_fd_ro = -1;
	dev_null_fd_wr = -1;

	rlist_foreach_entry_safe(handle, &popen_head, list, tmp) {
		/*
		 * If children are still running we should move
		 * them out of the pool and kill them all then.
		 * Note though that we don't do an explicit wait
		 * here, OS will reap them anyway, just release
		 * the memory occupied for popen handles.
		 */
		if (popen_may_pidop(handle))
			popen_unregister(handle);
		popen_delete(handle);
	}

	if (popen_pids_map) {
		mh_i32ptr_delete(popen_pids_map);
		popen_pids_map = NULL;
	}
}
