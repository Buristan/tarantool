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
#include "assoc.h"
#include "say.h"

/* Children pids map popen_handle map */
static struct mh_i32ptr_t *popen_pids_map = NULL;

/* All popen handles to be able to cleanup them on exit */
static RLIST_HEAD(popen_head);

/* /dev/null to be used inside children if requested */
static int dev_null_fd_ro = -1;
static int dev_null_fd_wr = -1;

/* To block SIGCHLD delivery when need a sync point */
static sigset_t popen_blockmask;

/*
 * In case if something really unexpected happened
 * and we no longer able to unblock SIGCHLD instead
 * of exiting with error in a middle of program work
 * we rather disable new popen openings leaving a user
 * a way to shutdown without loosing a memory data.
 */
static bool popen_blockmask_broken = false;

/**
 * popen_register - register popen handle in a pids map
 * @handle: a handle to register
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
 * @pid: pid of a handler
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
 * @handle: a handle to remove
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
 * handle_alloc - allocate new popen hanldle with flags specified
 * @flags: flags to be used
 *
 * Everything else initialized to default values.
 *
 * Returns pointer to a new popen or NULL on error.
 */
static struct popen_handle *
handle_alloc(unsigned int flags)
{
	struct popen_handle *handle;

	handle = malloc(sizeof(*handle));
	if (!handle) {
		say_syserror("popen: Can't allocate handle");
		return NULL;
	}

	handle->wstatus	= 0;
	handle->pid	= -1;
	handle->flags	= flags;

	rlist_create(&handle->list);

	/* all fds to -1 */
	memset(handle->fds, 0xff, sizeof(handle->fds));

	say_debug("popen: allocated %p", handle);
	return handle;
}

/**
 * handle_free - free memory allocated for a handle
 * @handle: a handle to free
 *
 * Just to match handle_alloc().
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
 * (setting an appropriate @errno).
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
 * (setting an appropriate @errno).
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
 * Returns 0 on succes, -1 otherwise.
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

	static_assert(lengthof(st->fds) == lengthof(st->fds),
		      "Statistics fds are screwed");

	memcpy(st->fds, handle->fds, sizeof(handle->fds));

	return 0;
}

/**
 * popen_write - write data to the child stdin
 * @handle:	popen handle
 * @buf:	data to write
 * @count:	number of bytes to write
 * @flags:	a flag representing stdin peer
 *
 * Returns number of bytes written or -1 on error.
 */
ssize_t
popen_write(struct popen_handle *handle, void *buf,
	    size_t count, unsigned int flags)
{
	if (!popen_may_io(handle, STDIN_FILENO, flags))
		return -1;

	say_debug("popen: %d: write idx %d",
		  handle->pid, STDIN_FILENO);

	return write(handle->fds[STDIN_FILENO], buf, count);
}

/**
 * popen_wait_read - wait for data appear on a child's peer
 * @handle:		popen handle
 * @fd:			peer fd to wait on
 * @timeout_msecs:	timeout in microseconds
 *
 * Returns 1 if there is data to read, -EAGAIN if timeout happened
 * and -1 on other errors setting errno accordingly.
 */
static int
popen_wait_read(struct popen_handle *handle, int fd, int timeout_msecs)
{
	struct pollfd pollfd = {
		.fd	= fd,
		.events	= POLLIN,
	};
	int ret;

	ret = poll(&pollfd, 1, timeout_msecs);
	say_debug("popen: %d: poll: ret %d fd %d events %#x revents %#x",
		  handle->pid, ret, fd, pollfd.events, pollfd.revents);

	if (ret == 1) {
		if (pollfd.revents == POLLIN) {
			return 1;
		} else {
			say_error("popen: %d: unexpected revents %#x",
				  handle->pid, pollfd.revents);
			return -EINVAL;
		}
	}

	return ret < 0 ? -errno : -EAGAIN;
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
 * Returns number of bytes read or -EAGAIN if @timeout_msecs expired.
 * On other errors -1 returned and errno set accordingly.
 */
ssize_t
popen_read_timeout(struct popen_handle *handle, void *buf,
		   size_t count, unsigned int flags,
		   int timeout_msecs)
{
	int idx, ret;

	idx = flags & POPEN_FLAG_FD_STDOUT ?
		STDOUT_FILENO : STDERR_FILENO;

	if (!popen_may_io(handle, idx, flags))
		return -1;

	say_debug("popen: %d: read idx %d fds %d timeout_msecs %d",
		  handle->pid, idx, handle->fds[idx], timeout_msecs);

	if (timeout_msecs > 0) {
		ret = popen_wait_read(handle, handle->fds[idx],
				      timeout_msecs);
		if (ret < 0) {
			if (ret != -EAGAIN) {
				errno = -ret;
				say_syserror("popen: %d: data wait failed",
					     handle->pid);
			}
			return ret;
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
 * is pretty heavy in performance.
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
 * __wstatus_str - shortcut to wstatus_str with static buffer
 * @wstatus:	status to encode
 *
 * Returns pointer to a buffer with encoded message.
 * Note this function uses the local static buffer thus
 * should not be called in parallel.
 */
static char *
__wstatus_str(int wstatus)
{
	static char buf[128];
	return wstatus_str(buf, sizeof(buf), wstatus);
}

/**
 * popen_notify_sigchld - notify popen subsisteb about SIGCHLD event
 * @pid:	PID of a process which changed its state
 * @wstatus:	signal status of a process
 *
 * The function is called from global SIGCHLD watcher in libev so
 * we need to figure out if it is our process which possibly been
 * terminated.
 *
 * Note the libev calls for wait() by self so we don't need to do
 * furter processing and reap children.
 */
static void
popen_notify_sigchld(pid_t pid, int wstatus)
{
	struct popen_handle *handle;
	static char buf[128];

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
 * ev_sigchld_cb - handle SIGCHLD from a child process.
 * @w:		a child exited
 * @revents:	unused
 */
static void
ev_sigchld_cb(EV_P_ ev_child *w, int revents)
{
	(void)revents;
	ev_child_stop(EV_A_ w);

	/*
	 * The reason for a separate helper is that
	 * we might need to notify more subsystems
	 * in future.
	 */
	popen_notify_sigchld(w->rpid, w->rstatus);
}

/**
 * popen_sigchld_block - block SIGCHLD
 * @oldmask:	a pointer where to save an old signal mask
 *
 * This routine is serialization point, we use signal blocking
 * to prevent concurrent access to popen handle from external
 * users which may kill programs by hands in any moment.
 *
 * Returns 0 on success, -1 otherwise.
 */
static int
popen_sigchld_block(sigset_t *oldmask)
{
	if (unlikely(popen_blockmask_broken)) {
		return 0;
	} else if (sigprocmask(SIG_BLOCK, &popen_blockmask, oldmask)) {
		say_syserror("popen: Can't block SIGCHLD");
		return -1;
	}
	return 0;
}

/**
 * popen_sigchld_block - unblock SIGCHLD
 * @oldmask:	a pointer to a mask to restore
 *
 * Returns 0 on success, -1 otherwise.
 */
static int
popen_sigchld_unblock(sigset_t *oldmask)
{
	if (unlikely(popen_blockmask_broken)) {
		return 0;
	} else if (sigprocmask(SIG_SETMASK, oldmask, NULL)) {
		say_syserror("popen: Can't unblock SIGCHLD");
		/*
		 * This is critial issue but give users
		 * an opportunity to shutdown.
		 */
		say_crit("popen: Signal handling is broken, "
			 "please consider restarting the program.");
		popen_blockmask_broken = true;
		return -1;
	}
	return 0;
}

/**
 * popen_wstatus_blocked - fetch popen child process wait status
 * @handle:	popen handle to inspect
 * @wstatus:	status to be filled if process exited
 * @early:	early bootstrap testing, don't print error if true
 *
 * The SIGCHLD must be blocked.
 *
 * Returns 1 if process changed its state filling
 * optional @wstatus, 0 if process is still running
 * and -1 on error.
 */
static inline int
popen_wstatus_blocked(struct popen_handle *handle,
		      int *wstatus, bool early)
{
	int pid;

	if (!popen_may_pidop(handle)) {
		if (handle && wstatus)
			*wstatus = handle->wstatus;
		/*
		 * Here is a trick if @handle is passed
		 * and its pid = -1 it means we already
		 * obtained sigchld so caller is interested
		 * in child status way after the child is
		 * finished.
		 */
		return handle ? 1 : -1;
	}

	pid = waitpid(handle->pid, &handle->wstatus, WNOHANG);
	if (pid == -1 && !early) {
		say_syserror("popen: Unable to wait pid %d (%s)",
			     handle->pid, __wstatus_str(handle->wstatus));
	} else if (pid > 0) {
		if (wstatus)
			*wstatus = handle->wstatus;
		pid = 1;
	}

	return pid;
}


/**
 * popen_wstatus - fetch popen child process wait status
 * @handle:	popen handle to inspect
 * @wstatus:	status to be filled if process exited
 *
 * Returns 1 if process changed its state filling
 * optional @wstatus, 0 if process is still running
 * and -1 on error.
 */
int
popen_wstatus(struct popen_handle *handle, int *wstatus)
{
	sigset_t oldmask;
	int ret;

	/*
	 * The pid in handle might be already killed
	 * by external signal or via natural exit of
	 * a program, so need to block.
	 */
	if (popen_sigchld_block(&oldmask))
		return -1;
	ret = popen_wstatus_blocked(handle, wstatus, false);
	popen_sigchld_unblock(&oldmask);

	return ret;
}

/**
 * popen_kill_blocked - kills a child process with signals blocked
 * @handle:	popen handle
 *
 * The SIGCHLD must be blocked.
 *
 * Returns 0 if child has been killed, -1 otherwise.
 */
static inline int
popen_kill_blocked(struct popen_handle *handle)
{
	int ret;

	/*
	 * A child may be killed or exited already.
	 */
	if (popen_may_pidop(handle)) {
		say_debug("popen: killpg %d", handle->pid);
		ret = killpg(handle->pid, SIGKILL);
		if (ret < 0) {
			say_syserror("popen: Unable to kill %d",
				     handle->pid);
		}
	} else
		ret = -1;

	return ret;
}

/**
 * popen_kill - kills a child process associated with popen handle
 * @handle:	popen handle
 *
 * Returns 0 if child has been killed, -1 otherwise.
 */
int
popen_kill(struct popen_handle *handle)
{
	sigset_t oldmask;
	int ret;

	if (popen_sigchld_block(&oldmask))
		return -1;
	ret = popen_kill_blocked(handle);

	popen_sigchld_unblock(&oldmask);
	return ret;
}

/**
 * popen_destroy_blocked - destory a popen handle
 * @handle:	a popen handle to destroy
 *
 * The function kills a child process and
 * close all fds and remove the handle from
 * a living list and finally frees the handle.
 *
 * The SIGCHLD must be blocked or the handle
 * must be not registered yet.
 *
 * Returns 0 on success, -1 otherwise.
 */
static inline int
popen_destroy_blocked(struct popen_handle *handle)
{
	size_t i;

	if (popen_kill(handle) && errno != ESRCH)
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
	handle_free(handle);
	return 0;
}

/**
 * popen_destroy - destory a popen handle
 * @handle:	pointer to a popen handle
 *
 * The function kills a child process associated with the
 * popen handle, closes all pipes and frees memory.
 *
 * After this call the popen object no longer usable.
 *
 * Returns 0 on succsess, -1 otherwise.
 */
int
popen_destroy(struct popen_handle *handle)
{
	sigset_t oldmask;
	int ret;

	if (!handle) {
		errno = ESRCH;
		return -1;
	}

	if (popen_sigchld_block(&oldmask))
		return -1;
	ret = popen_destroy_blocked(handle);
	popen_sigchld_unblock(&oldmask);
	return ret;
}

/**
 * create_pipe - create nonblocking cloexec pipe
 * @pfd:	pipe ends to setup
 *
 * Returns 0 on success, -1 on error.
 */
static int
create_pipe(int pfd[2])
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
#endif
	return 0;
}

/**
 * popen_create - Create new popen handle
 * @command:	a command to run inside child process
 * @flags:	child pipe ends specification
 *
 * This function creates a new child process and passes it
 * pipe ends to communicate with child's stdin/stdout/stderr
 * depending on @flags. Where @flags could be the bitwise or
 * for the following values:
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
 * These flags do have no effect if appropriate POPEN_FLAG_FD_STDx
 * flags are set.
 *
 * When need to completely close the descriptors the
 * following values can be used:
 *
 *  POPEN_FLAG_FD_STDIN_CLOSE
 *  POPEN_FLAG_FD_STDOUT_CLOSE
 *  POPEN_FLAG_FD_STDERR_CLOSE
 *
 * These flags do have no effect if appropriate POPEN_FLAG_FD_STDx
 * flags are set.
 *
 * If none of POPEN_FLAG_FD_STDx flags are specified the child
 * process will run with all files inherited from a parent.
 *
 * By default the @command is executed via "sh -c". To execute
 * @command directly use the POPEN_FLAG_NOSHELL flag.
 *
 * Returns pointer to new popen handle on success,
 * otherwise NULL returned.
 */
struct popen_handle *
popen_create(const char *command, unsigned int flags)
{
	struct popen_handle *handle = NULL;

	char * const argv_native[] = {
		(char *)command, NULL,
	};
        char * const argv_sh[] = {
		(char *)"sh", (char *)"-c",
		(char *)command, NULL,
	};
	/*
	 * FIXME: Need to pass env in arguments?
	 * Better discuss with a team.
	 */
	char * const envp[] = { };

	int pfd[POPEN_FLAG_FD_STDEND_BIT][2] = {
		{-1, -1}, {-1, -1}, {-1, -1},
	};

	int saved_errno;
	int ret = -1;
	pid_t pid;
	size_t i;

	static const struct {
		unsigned int	mask;
		unsigned int	mask_devnull;
		unsigned int	mask_close;
		int		fileno;
		int		*dev_null_fd;
		int		parent_idx;
		int		child_idx;
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

	sigset_t oldmask;

	say_debug("popen: command \"%s\" flags %#x", command, flags);

	if (!command) {
		errno = EINVAL;
		say_syserror("popen: No command provided");
		return NULL;
	}

	/*
	 * If sometime earlier we've been unable
	 * to unblock signals don't allow to create
	 * new pipes, the system is unstable.
	 */
	if (unlikely(popen_blockmask_broken)) {
		errno = EINVAL;
		say_error("popen: Service unavailable");
		return NULL;
	}

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

	handle = handle_alloc(flags);
	if (!handle)
		return NULL;

	skip_fds[nr_skip_fds++] = dev_null_fd_ro;
	skip_fds[nr_skip_fds++] = dev_null_fd_wr;
	assert(nr_skip_fds <= lengthof(skip_fds));

	for (i = 0; i < lengthof(pfd_map); i++) {
		if (flags & pfd_map[i].mask) {
			if (create_pipe(pfd[i]))
				goto out_err;

			skip_fds[nr_skip_fds++] = pfd[i][0];
			skip_fds[nr_skip_fds++] = pfd[i][1];
			assert(nr_skip_fds <= lengthof(skip_fds));

			say_debug("popen: created pipe %d [%d:%d]",
				  i, pfd[i][0], pfd[i][1]);
		} else if (!(flags & pfd_map[i].mask_devnull) &&
			   !(flags & pfd_map[i].mask_close)) {
			skip_fds[nr_skip_fds++] = pfd_map[i].fileno;

			say_debug("popen: inherit fd %d",
				  pfd_map[i].fileno);
		}
	}

	/*
	 * Need to block signals so we won't hit
	 * a race where child process exit early
	 * and this pid will get reused by someone
	 * else (remember the libev wait() by self).
	 */
	if (popen_sigchld_block(&oldmask)) {
		say_syserror("popen: Unable to block SIGCHLD");
		goto out_err;
	}

	/*
	 * We have to use vfork here because libev has own
	 * at_fork helpers with mutex, so we will have double
	 * lock here and stuck forever otherwise.
	 *
	 * The good news that this affect coio only the
	 * other tarantoll threads are not waiting for
	 * vfork to complete.
	 */
	handle->pid = vfork();
	if (handle->pid < 0) {
		goto out_err_unblock;
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
		 * We have to be a session leader otherwise
		 * won't be able to kill a group of children.
		 */
		ret = setsid();
		if (ret < 0)
			_exit(errno);

		ret = close_inherited_fds(skip_fds, nr_skip_fds);
		if (ret)
			_exit(errno);

		for (i = 0; !ret && i < lengthof(pfd_map); i++) {
			int fileno = pfd_map[i].fileno;
			if (flags & pfd_map[i].mask) {
				int child_idx = pfd_map[i].child_idx;

				/* put child peer end at known place */
				if (dup2(pfd[i][child_idx], fileno) < 0) {
					ret = errno;
					continue;
				}

				/* parent's pipe no longer needed */
				if (close(pfd[i][0])) {
					ret = errno;
					continue;
				} else if (close(pfd[i][1])) {
					ret = errno;
					continue;
				}
			} else {
				/* Use /dev/null if requested */
				if (flags & pfd_map[i].mask_devnull) {
					if (dup2(*pfd_map[i].dev_null_fd, fileno) < 0) {
						ret = errno;
						continue;
					}
				}

				/* Or close the destination completely */
				if (flags & pfd_map[i].mask_close) {
					if (close(fileno) && errno != EBADF) {
						ret = errno;
						continue;
					}
				}

				/* Otherwise inherit from a parent */
			}
		}

		if (close(dev_null_fd_ro))
			ret = errno;
		else if (close(dev_null_fd_wr))
			ret = errno;

		if (!ret) {
			if (flags & POPEN_FLAG_SHELL)
				ret = execve(_PATH_BSHELL, argv_sh, envp);
			else
				ret = execve(command, argv_native, envp);
		}
		_exit(ret);
		unreachable();
	}

	for (i = 0; i < lengthof(pfd_map); i++) {
		if (flags & pfd_map[i].mask) {
			int parent_idx = pfd_map[i].parent_idx;
			int child_idx = pfd_map[i].child_idx;

			handle->fds[i] = pfd[i][parent_idx];
			say_debug("popen: keep pipe %d [%d]",
				  i, handle->fds[i]);

			if (close(pfd[i][child_idx]))
				goto out_err_unblock;

			pfd[i][child_idx] = -1;
		}
	}

	pid = popen_wstatus_blocked(handle, NULL, true);
	if (pid == -1) {
		say_debug("popen: Child %d exited early",
			  handle->pid);
		handle->pid = -1;
	} else if (pid == 1) {
		bool exited = WIFEXITED(handle->wstatus);
		bool signaled = WIFSIGNALED(handle->wstatus);

		if (exited || signaled) {
			say_debug("popen: Child %d %s with %d",
				  pid, exited ? "exited" : "signaled",
				  exited ? WEXITSTATUS(handle->wstatus) :
				  WTERMSIG(handle->wstatus));
			handle->pid = -1;
		}
	}

	/*
	 * Link it into global list for force
	 * cleanup on exit.
	 */
	rlist_add(&popen_head, &handle->list);

	if (handle->pid != -1) {
		/*
		 * To watch when a child get exited.
		 */
		popen_register(handle);

		say_debug("popen: ev_child_start %d", handle->pid);
		ev_child_init(&handle->ev_sigchld, ev_sigchld_cb, pid, 0);
		ev_child_start(EV_DEFAULT_ &handle->ev_sigchld);
	}

	say_debug("popen: created child %d", handle->pid);

	popen_sigchld_unblock(&oldmask);
	return handle;

out_err_unblock:
	saved_errno = errno;
	popen_sigchld_unblock(&oldmask);
	errno = saved_errno;
out_err:
	saved_errno = errno;
	popen_destroy(handle);
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

	say_debug("popen: initialize");
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
	}

	sigemptyset(&popen_blockmask);
	sigaddset(&popen_blockmask, SIGCHLD);
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
 * popen_fini - finalize popen subsystem
 *
 * Kills all running children and frees resources.
 */
void
popen_fini(void)
{
	struct popen_handle *handle, *tmp;
	sigset_t oldmask;

	say_debug("popen: finalize");

	close(dev_null_fd_ro);
	close(dev_null_fd_wr);
	dev_null_fd_ro = -1;
	dev_null_fd_wr = -1;

	if (popen_sigchld_block(&oldmask))
		return;

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
		popen_destroy_blocked(handle);
	}

	popen_sigchld_unblock(&oldmask);

	if (popen_pids_map) {
		mh_i32ptr_delete(popen_pids_map);
		popen_pids_map = NULL;
	}
}
