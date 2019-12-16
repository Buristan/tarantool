/*
 * Copyright 2010-2019, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "diag.h"
#include "core/popen.h"

#include "lua/utils.h"
#include "lua/popen.h"

static inline int
lbox_pushsyserror(struct lua_State *L)
{
	diag_set(SystemError, "popen: %s", strerror(errno));
	return luaT_push_nil_and_error(L);
}

static inline int
lbox_push_error(struct lua_State *L)
{
	diag_set(SystemError, "popen: %s", strerror(errno));
	struct error *e = diag_last_error(diag_get());
	assert(e != NULL);
	luaT_pusherror(L, e);
	return 1;
}

static inline int
lbox_pushbool(struct lua_State *L, bool res)
{
	lua_pushboolean(L, res);
	if (!res) {
		lbox_push_error(L);
		return 2;
	}
	return 1;
}

/**
 * lbox_fio_popen_new - creates a new popen handle and runs a command inside
 * @command:	a command to run
 * @flags:	popen_flag_bits
 *
 * Returns pair @handle = data, @err = nil on success,
 * @handle = nil, err ~= nil on error.
 */
static int
lbox_popen_new(struct lua_State *L)
{
	struct popen_handle *handle;
	struct popen_opts opts = { };
	size_t i, argv_size;
	ssize_t nr_env;

	if (lua_gettop(L) < 1 || !lua_istable(L, 1))
		luaL_error(L, "Usage: fio.run({opts}])");

	lua_pushstring(L, "argv");
	lua_gettable(L, -2);
	if (!lua_istable(L, -1))
		luaL_error(L, "fio.run: {argv=...} is not a table");
	lua_pop(L, 1);

	lua_pushstring(L, "flags");
	lua_gettable(L, -2);
	if (!lua_isnumber(L, -1))
		luaL_error(L, "fio.run: {flags=...} is not a number");
	opts.flags = lua_tonumber(L, -1);
	lua_pop(L, 1);

	lua_pushstring(L, "argc");
	lua_gettable(L, -2);
	if (!lua_isnumber(L, -1))
		luaL_error(L, "fio.run: {argc=...} is not a number");
	opts.nr_argv = lua_tonumber(L, -1);
	lua_pop(L, 1);

	if (opts.nr_argv < 1)
		luaL_error(L, "fio.run: {argc} is too small");

	/*
	 * argv array should contain NULL element at the
	 * end and probably "sh", "-c" at the start, so
	 * reserve enough space for any flags.
	 */
	opts.nr_argv += 3;
	argv_size = sizeof(char *) * opts.nr_argv;
	opts.argv = malloc(argv_size);
	if (!opts.argv)
		luaL_error(L, "fio.run: can't allocate argv");

	lua_pushstring(L, "argv");
	lua_gettable(L, -2);
	lua_pushnil(L);
	for (i = 2; lua_next(L, -2) != 0; i++) {
		assert(i < opts.nr_argv);
		opts.argv[i] = (char *)lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	opts.argv[0] = NULL;
	opts.argv[1] = NULL;
	opts.argv[opts.nr_argv - 1] = NULL;

	/*
	 * Environment can be filled, empty
	 * to inherit or contain one NULL to
	 * be zapped.
	 */
	lua_pushstring(L, "envc");
	lua_gettable(L, -2);
	if (!lua_isnumber(L, -1)) {
		free(opts.argv);
		luaL_error(L, "fio.run: {envc=...} is not a number");
	}
	nr_env = lua_tonumber(L, -1);
	lua_pop(L, 1);

	if (nr_env >= 0) {
		/* Should be NULL terminating */
		opts.env = malloc((nr_env + 1) * sizeof(char *));
		if (!opts.env) {
			free(opts.argv);
			luaL_error(L, "fio.run: can't allocate env");
		}

		lua_pushstring(L, "env");
		lua_gettable(L, -2);
		if (!lua_istable(L, -1)) {
			free(opts.argv);
			free(opts.env);
			luaL_error(L, "fio.run: {env=...} is not a table");
		}
		lua_pushnil(L);
		for (i = 0; lua_next(L, -2) != 0; i++) {
			assert((ssize_t)i <= nr_env);
			opts.env[i] = (char *)lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);

		opts.env[nr_env] = NULL;
	} else {
		/*
		 * Just zap it to nil, the popen will
		 * process inheriting by self.
		 */
		opts.env = NULL;
	}

	handle = popen_new(&opts);

	free(opts.argv);
	free(opts.env);

	if (!handle)
		return lbox_pushsyserror(L);

	lua_pushlightuserdata(L, handle);
	return 1;
}

/**
 * lbox_fio_popen_kill - kill popen's child process
 * @handle:	a handle carries child process to kill
 *
 * Returns true if process is killed and false
 * otherwise. Note the process is simply signaled
 * and it doesn't mean it is killed immediately,
 * Poll lbox_fio_pstatus if need to find out when
 * exactly the child is reaped out.
 */
static int
lbox_popen_kill(struct lua_State *L)
{
	struct popen_handle *p = lua_touserdata(L, 1);
	return lbox_pushbool(L, popen_send_signal(p, SIGKILL) == 0);
}

/**
 * lbox_fio_popen_term - terminate popen's child process
 * @handle:	a handle carries child process to terminate
 *
 * Returns true if process is terminated and false
 * otherwise.
 */
static int
lbox_popen_term(struct lua_State *L)
{
	struct popen_handle *p = lua_touserdata(L, 1);
	return lbox_pushbool(L, popen_send_signal(p, SIGTERM) == 0);
}

/**
 * lbox_fio_popen_signal - send signal to a child process
 * @handle:	a handle carries child process to terminate
 * @signo:	signal number to send
 *
 * Returns true if signal is sent.
 */
static int
lbox_popen_signal(struct lua_State *L)
{
	struct popen_handle *p = lua_touserdata(L, 1);
	int signo = lua_tonumber(L, 2);
	return lbox_pushbool(L, popen_send_signal(p, signo) == 0);
}

/**
 * lbox_popen_state - fetch popen child process status
 * @handle:	a handle to fetch status from
 *
 * Returns @err = nil, @reason = POPEN_STATE_x,
 * @exit_code = 'number' on success, @err ~= nil on error.
 */
static int
lbox_popen_state(struct lua_State *L)
{
	struct popen_handle *p = lua_touserdata(L, 1);
	int state, exit_code, ret;

	ret = popen_state(p, &state, &exit_code);
	if (ret < 0)
		return lbox_push_error(L);

	lua_pushnil(L);
	lua_pushinteger(L, state);
	lua_pushinteger(L, exit_code);
	return 3;
}

/**
 * lbox_popen_read - read data from a child peer
 * @handle:	handle of a child process
 * @buf:	destination buffer
 * @count:	number of bytes to read
 * @flags:	which peer to read (stdout,stderr)
 * @timeout:	timeout in seconds; ignored if negative
 *
 * Returns @size = 'read bytes', @err = nil on success,
 * @size = nil, @err ~= nil on error.
 */
static int
lbox_popen_read(struct lua_State *L)
{
	struct popen_handle *handle = lua_touserdata(L, 1);
	uint32_t ctypeid;
	void *buf =  *(char **)luaL_checkcdata(L, 2, &ctypeid);
	size_t count = lua_tonumber(L, 3);
	unsigned int flags = lua_tonumber(L, 4);
	ev_tstamp timeout = lua_tonumber(L, 5);
	ssize_t ret;

	ret = popen_read_timeout(handle, buf, count,
				 flags, timeout);
	if (ret < 0)
		return lbox_pushsyserror(L);

	lua_pushinteger(L, ret);
	return 1;
}

/**
 * lbox_popen_write - write data to a child peer
 * @handle:	a handle of a child process
 * @buf:	source buffer
 * @count:	number of bytes to write
 * @flags:	which peer to write (stdin)
 * @timeout:	timeout in seconds; ignored if negative
 *
 * Returns @err = nil on succes, @err ~= nil on error.
 */
static int
lbox_popen_write(struct lua_State *L)
{
	struct popen_handle *handle = lua_touserdata(L, 1);
	void *buf = (void *)lua_tostring(L, 2);
	uint32_t ctypeid = 0;
	if (buf == NULL)
		buf =  *(char **)luaL_checkcdata(L, 2, &ctypeid);
	size_t count = lua_tonumber(L, 3);
	unsigned int flags = lua_tonumber(L, 4);
	ev_tstamp timeout = lua_tonumber(L, 5);
	ssize_t ret;

	ret = popen_write_timeout(handle, buf, count, flags, timeout);
	if (ret < 0)
		return lbox_pushsyserror(L);
	return lbox_pushbool(L, ret == 0);
}

/**
 * lbox_popen_info - return information about popen handle
 * @handle:	a handle of a child process
 *
 * Returns a @table ~= nil, @err = nil on success,
 * @table = nil, @err ~= nil on error.
 */
static int
lbox_popen_info(struct lua_State *L)
{
	struct popen_handle *handle = lua_touserdata(L, 1);
	int state, exit_code, ret;
	struct popen_stat st = { };

	if (popen_stat(handle, &st))
		return lbox_pushsyserror(L);

	ret = popen_state(handle, &state, &exit_code);
	if (ret < 0)
		return lbox_pushsyserror(L);

	assert(state < POPEN_STATE_MAX);

	lua_newtable(L);

	lua_pushliteral(L, "pid");
	lua_pushinteger(L, st.pid);
	lua_settable(L, -3);

	lua_pushliteral(L, "command");
	lua_pushstring(L, popen_command(handle));
	lua_settable(L, -3);

	lua_pushliteral(L, "flags");
	lua_pushinteger(L, st.flags);
	lua_settable(L, -3);

	lua_pushliteral(L, "state");
	lua_pushstring(L, popen_state_str(state));
	lua_settable(L, -3);

	lua_pushliteral(L, "exit_code");
	lua_pushinteger(L, exit_code);
	lua_settable(L, -3);

	lua_pushliteral(L, "stdin");
	lua_pushinteger(L, st.fds[STDIN_FILENO]);
	lua_settable(L, -3);

	lua_pushliteral(L, "stdout");
	lua_pushinteger(L, st.fds[STDOUT_FILENO]);
	lua_settable(L, -3);

	lua_pushliteral(L, "stderr");
	lua_pushinteger(L, st.fds[STDERR_FILENO]);
	lua_settable(L, -3);

	return 1;
}

/**
 * lbox_popen_delete - close a popen handle
 * @handle:	a handle to close
 *
 * If there is a running child it get killed first.
 *
 * Returns true if a handle is closeed, false otherwise.
 */
static int
lbox_popen_delete(struct lua_State *L)
{
	void *handle = lua_touserdata(L, 1);
	return lbox_pushbool(L, popen_delete(handle) == 0);
}

/**
 * tarantool_lua_popen_init - Create popen methods
 */
void
tarantool_lua_popen_init(struct lua_State *L)
{
	static const struct luaL_Reg popen_methods[] = {
		{ },
	};

	/* public methods */
	luaL_register_module(L, "popen", popen_methods);

	static const struct luaL_Reg builtin_methods[] = {
		{ "new",		lbox_popen_new,		},
		{ "delete",		lbox_popen_delete,	},
		{ "kill",		lbox_popen_kill,	},
		{ "term",		lbox_popen_term,	},
		{ "signal",		lbox_popen_signal,	},
		{ "state",		lbox_popen_state,	},
		{ "read",		lbox_popen_read,	},
		{ "write",		lbox_popen_write,	},
		{ "info",		lbox_popen_info,	},
		{ },
	};

	/* builtin methods */
	lua_pushliteral(L, "builtin");
	lua_newtable(L);

	luaL_register(L, NULL, builtin_methods);
	lua_settable(L, -3);

	/*
	 * Popen constants.
	 */
#define lua_gen_const(_n, _v)		\
	lua_pushliteral(L, _n);		\
	lua_pushinteger(L, _v);		\
	lua_settable(L, -3)

	lua_pushliteral(L, "c");
	lua_newtable(L);

	/*
	 * Flag masks.
	 */
	lua_pushliteral(L, "flag");
	lua_newtable(L);

	lua_gen_const("NONE",			POPEN_FLAG_NONE);

	lua_gen_const("STDIN",			POPEN_FLAG_FD_STDIN);
	lua_gen_const("STDOUT",			POPEN_FLAG_FD_STDOUT);
	lua_gen_const("STDERR",			POPEN_FLAG_FD_STDERR);

	lua_gen_const("STDIN_DEVNULL",		POPEN_FLAG_FD_STDIN_DEVNULL);
	lua_gen_const("STDOUT_DEVNULL",		POPEN_FLAG_FD_STDOUT_DEVNULL);
	lua_gen_const("STDERR_DEVNULL",		POPEN_FLAG_FD_STDERR_DEVNULL);

	lua_gen_const("STDIN_CLOSE",		POPEN_FLAG_FD_STDIN_CLOSE);
	lua_gen_const("STDOUT_CLOSE",		POPEN_FLAG_FD_STDOUT_CLOSE);
	lua_gen_const("STDERR_CLOSE",		POPEN_FLAG_FD_STDERR_CLOSE);

	lua_gen_const("SHELL",			POPEN_FLAG_SHELL);
	lua_gen_const("SETSID",			POPEN_FLAGS_SETSID);
	lua_gen_const("CLOSE_FDS",		POPEN_FLAG_CLOSE_FDS);
	lua_gen_const("RESTORE_SIGNALS",	POPEN_FLAGS_RESTORE_SIGNALS);
	lua_settable(L, -3);

	lua_pushliteral(L, "state");
	lua_newtable(L);

	lua_gen_const("ALIVE",			POPEN_STATE_ALIVE);
	lua_gen_const("EXITED",			POPEN_STATE_EXITED);
	lua_gen_const("SIGNALED",		POPEN_STATE_SIGNALED);
	lua_settable(L, -3);

#undef lua_gen_const

	lua_settable(L, -3);
	lua_pop(L, 1);
}
