-- popen.lua (builtin file)
--
-- vim: ts=4 sw=4 et

local buffer = require('buffer')
local popen = require('popen')
local fiber = require('fiber')
local ffi = require('ffi')
local bit = require('bit')

local const_char_ptr_t = ffi.typeof('const char *')

local builtin = popen.builtin
popen.builtin = nil

local popen_methods = { }

local function default_flags()
    local flags = popen.c.flag.NONE

    -- default flags: close everything and use shell
    flags = bit.bor(flags, popen.c.flag.STDIN_CLOSE)
    flags = bit.bor(flags, popen.c.flag.STDOUT_CLOSE)
    flags = bit.bor(flags, popen.c.flag.STDERR_CLOSE)
    flags = bit.bor(flags, popen.c.flag.SHELL)
    flags = bit.bor(flags, popen.c.flag.SETSID)
    flags = bit.bor(flags, popen.c.flag.CLOSE_FDS)
    flags = bit.bor(flags, popen.c.flag.RESTORE_SIGNALS)

    return flags
end

--
-- A map for popen option keys into tfn ('true|false|nil') values
-- where bits are just set without additional manipulations.
--
-- For example stdin=true means to open stdin end for write,
-- stdin=false to close the end, finally stdin[=nil] means to
-- provide /dev/null into a child peer.
local flags_map_tfn = {
    stdin = {
        popen.c.flag.STDIN,
        popen.c.flag.STDIN_CLOSE,
        popen.c.flag.STDIN_DEVNULL,
    },
    stdout = {
        popen.c.flag.STDOUT,
        popen.c.flag.STDOUT_CLOSE,
        popen.c.flag.STDOUT_DEVNULL,
    },
    stderr = {
        popen.c.flag.STDERR,
        popen.c.flag.STDERR_CLOSE,
        popen.c.flag.STDERR_DEVNULL,
    },
}

--
-- A map for popen option keys into tf ('true|false') values
-- where bits are set on 'true' and clear on 'false'.
local flags_map_tf = {
    shell = {
        popen.c.flag.SHELL,
    },
    close_fds = {
        popen.c.flag.CLOSE_FDS
    },
    restore_signals = {
        popen.c.flag.RESTORE_SIGNALS
    },
    start_new_session = {
        popen.c.flag.SETSID
    },
}

--
-- Parses flags options from flags_map_tfn and
-- flags_map_tf tables.
local function parse_flags(epfx, flags, opts)
    if opts == nil then
        return flags
    end
    for k,v in pairs(opts) do
        if flags_map_tfn[k] == nil then
            if flags_map_tf[k] == nil then
                error(string.format("%s: Unknown key %s", epfx, k))
            end
            if v == true then
                flags = bit.bor(flags, flags_map_tf[k][1])
            elseif v == false then
                flags = bit.band(flags, bit.bnot(flags_map_tf[k][1]))
            else
                error(string.format("%s: Unknown value %s", epfx, v))
            end
        else
            if v == true then
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][2]))
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][3]))
                flags = bit.bor(flags, flags_map_tfn[k][1])
            elseif v == false then
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][1]))
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][3]))
                flags = bit.bor(flags, flags_map_tfn[k][2])
            elseif v == "devnull" then
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][1]))
                flags = bit.band(flags, bit.bnot(flags_map_tfn[k][2]))
                flags = bit.bor(flags, flags_map_tfn[k][3])
            else
                error(string.format("%s: Unknown value %s", epfx, v))
            end
        end
    end
    return flags
end

--
-- Parse "mode" string to flags
local function parse_mode(epfx, flags, mode)
    if mode == nil then
        return flags
    end
    for i = 1, #mode do
        local c = mode:sub(i, i)
        if c == 'r' then
            flags = bit.band(flags, bit.bnot(popen.c.flag.STDOUT_CLOSE))
            flags = bit.bor(flags, popen.c.flag.STDOUT)
            flags = bit.band(flags, bit.bnot(popen.c.flag.STDERR_CLOSE))
            flags = bit.bor(flags, popen.c.flag.STDERR)
        elseif c == 'w' then
            flags = bit.band(flags, bit.bnot(popen.c.flag.STDIN_CLOSE))
            flags = bit.bor(flags, popen.c.flag.STDIN)
        else
            error(string.format("%s: Unknown mode %s", epfx, c))
        end
    end
    return flags
end

--
-- Close a popen object and release all resources.
-- In case if there is a running child process
-- it will be killed.
--
-- Returns @ret = true if popen object is closed, and
-- @ret = false, @err ~= nil on error.
popen_methods.close = function(self)
    local ret, err = builtin.delete(self.cdata)
    if err ~= nil then
        return false, err
    end
    self.cdata = nil
    return true
end

--
-- Kill a child process
--
-- Returns @ret = true on success,
-- @ret = false, @err ~= nil on error.
popen_methods.kill = function(self)
    return builtin.kill(self.cdata)
end

--
-- Terminate a child process
--
-- Returns @ret = true on success,
-- @ret = false, @err ~= nil on error.
popen_methods.terminate = function(self)
    return builtin.term(self.cdata)
end

--
-- Send signal with number @signo to a child process
--
-- Returns @ret = true on success,
-- @ret = false, @err ~= nil on error.
popen_methods.send_signal = function(self, signo)
    return builtin.signal(self.cdata, signo)
end

--
-- Fetch a child process state
--
-- Returns @err = nil, @state = popen.c.state, @exit_code = num,
-- otherwise @err ~= nil.
popen_methods.state = function(self)
    return builtin.state(self.cdata)
end

--
-- Wait until a child process get exited.
--
-- Returns the same as popen_methods.status.
popen_methods.wait = function(self)
    local err, state, code
    while true do
        err, state, code = builtin.state(self.cdata)
        if err or state ~= popen.c.state.ALIVE then
            break
        end
        fiber.sleep(self.wait_secs)
    end
    return err, state, code
end

--
-- popen:read2 - read a stream of a child process
-- @opts:       options table
--
-- The options should have the following keys
--
-- @buf:        const_char_ptr_t buffer
-- @size:       size of the buffer
-- @flags:      stdout=true or stderr=true
-- @timeout:    read timeout in seconds, < 0 to ignore
--
-- Returns @res = bytes, err = nil in case if read processed
-- without errors, @res = nil, @err ~= nil otherwise.
popen_methods.read2 = function(self, opts)
    local flags = parse_flags("popen:read2",
                              popen.c.flag.NONE,
                              opts['flags'])
    local timeout = -1

    if opts['buf'] == nil then
        error("popen:read2 {'buf'} key is missed")
    elseif opts['size'] == nil then
        error("popen:read2 {'size'} key is missed")
    elseif opts['timeout'] ~= nil then
        timeout = tonumber(opts['timeout'])
    end

    return builtin.read(self.cdata, opts['buf'],
                        tonumber(opts['size']),
                        flags, timeout)
end

--
-- popen:write2 - write to a child's streem
-- @opts:       options table
--
-- The options should have the following keys
--
-- @buf:        const_char_ptr_t buffer
-- @size:       size of the buffer
-- @flags:      stdin=true
-- @timeout:    write timeout in seconds, < 0 to ignore
--
-- Returns @err = nil on success, @err ~= nil otherwise.
popen_methods.write2 = function(self, opts)
    local flags = parse_flags("popen:write2",
                              popen.c.flag.NONE,
                              opts['flags'])
    local timeout = -1

    if opts['buf'] == nil then
        error("popen:write2 {'buf'} key is missed")
    elseif opts['size'] == nil then
        error("popen:write2 {'size'} key is missed")
    elseif opts['timeout'] ~= nil then
        timeout = tonumber(opts['timeout'])
    end

    return builtin.write(self.cdata, opts['buf'],
                         tonumber(opts['size']),
                         flags, timeout)
end

--
-- popen:read - read string from a stream
-- @stderr:     set to true to read from stderr, optional
-- @timeout:    timeout in seconds, optional
--
-- Returns a result string, or res = nil, @err ~= nil on error.
popen_methods.read = function(self, stderr, timeout)
    local ibuf = buffer.ibuf()
    local buf = ibuf:reserve(self.read_size)
    local flags

    if stderr ~= nil then
        flags = { stderr = true }
    else
        flags = { stdout = true }
    end

    if timeout == nil then
        timeout = -1
    end

    local res, err = self:read2({
        buf     = buf,
        size    = self.read_size,
        flags   = flags,
        timeout = timeout,
    })

    if err ~= nil then
        ibuf:recycle()
        return nil, err
    end

    ibuf:alloc(res)
    res = ffi.string(ibuf.rpos, ibuf:size())
    ibuf:recycle()

    return res
end

--
-- popen:write - write string @str to stdin stream
-- @str:        string to write
-- @timeout:    timeout in seconds, optional
--
-- Returns @err = nil on success, @err ~= nil on error.
popen_methods.write = function(self, str, timeout)
    if timeout == nil then
        timeout = -1
    end
    return self:write2({
        buf     = str,
        size    = #str,
        flags   = { stdin = true },
        timeout = timeout,
    })
end

--
-- popen:info -- obtain information about a popen object
--
-- Returns @info table, err == nil on success,
-- @info = nil, @err ~= nil otherwise.
--
-- The @info table contains the following keys:
--
-- @pid:        pid of a child process
-- @flags:      flags associated (popen.c.flag bitmap)
-- @stdout:     parent peer for stdout, -1 if closed
-- @stderr:     parent peer for stderr, -1 if closed
-- @stdin:      parent peer for stdin, -1 if closed
-- @state:      alive | exited | signaled
-- @exit_code:  exit code of a child process if been
--              exited or killed by a signal
--
-- The child should never be in "unknown" state, reserved
-- for unexpected errors.
--
popen_methods.info = function(self)
    return builtin.info(self.cdata)
end

--
-- Create a new popen object from options
local function popen_reify(opts)
    local cdata, err = builtin.new(opts)
    if err ~= nil then
        return nil, err
    end

    local handle = {
        -- a handle itself for future use
        cdata           = cdata,

        -- sleeping period for the @wait method
        wait_secs       = 0.3,

        -- size of a read buffer to allocate
        -- in case of implicit read, this number
        -- is taken from luatest repo to fit the
        -- value people are familiar with
        read_size       = 4096,
    }

    setmetatable(handle, {
        __index     = popen_methods,
    })

    return handle
end

--
-- popen.posix - create a new child process and execute a command inside
-- @command:    a command to run
-- @mode:       r - to grab child's stdout for read
--              (stderr kept opened as well for 2>&1 redirection)
--              w - to obtain stdin for write
--
-- Note: Since there are two options only the following parameters
-- are implied (see default_flags): all fds except stdin/out/err
-- are closed, signals are restored to default handlers, the command
-- is executed in a new session.
--
-- Examples:
--
--  ph = require('popen').posix("date", "r")
--      runs date as "sh -c date" to read the output,
--      closing all file descriptors except stdout/err
--      inside a child process
--
popen.posix = function(command, mode)
    local flags = default_flags()

    if type(command) ~= 'string' then
        error("Usage: popen.posix(command[, rw])")
    end

    -- Mode gives simplified flags
    flags = parse_mode("popen.posix", flags, mode)

    local opts = {
        argv    = { command },
        argc    = 1,
        flags   = flags,
        envc    = -1,
    }

    return popen_reify(opts)
end

-- popen.popen - execute a child program in a new process
-- @opt:    options table
--
-- @opts carries of the following options:
--
--  @argv:  an array of a program to run with
--          command line options, mandatory
--
--  @env:   an array of environment variables to
--          be used inside a process, if not
--          set then the current environment is
--          inherited, if set to an empty array
--          then the environment will be dropped
--
--  @flags: a dictionary to describe communication pipes
--          and other parameters of a process to run
--
--      stdin=true      to write into STDIN_FILENO of a child process
--      stdin=false     to close STDIN_FILENO inside a child process [*]
--      stdin="devnull" a child will get /dev/null as STDIN_FILENO
--
--      stdout=true     to read STDOUT_FILENO of a child process
--      stdout=false    to close STDOUT_FILENO inside a child process [*]
--      stdout="devnull" a child will get /dev/null as STDOUT_FILENO
--
--      stderr=true     to read STDERR_FILENO from a child process
--      stderr=false    to close STDERR_FILENO inside a child process [*]
--      stderr="devnull" a child will get /dev/null as STDERR_FILENO
--
--      shell=true      runs a child process via "sh -c" [*]
--      shell=false     invokes a child process executable directly
--
--      close_fds=true  close all inherited fds from a parent [*]
--
--      restore_signals=true
--                      all signals modified by a caller reset
--                      to default handler [*]
--
--      start_new_session=true
--                      start executable inside a new session [*]
--
-- [*] are default values
--
-- Examples:
--
--  ph = require('popen').popen({argv = {"date"}, flags = {stdout=true}})
--  ph:read()
--  ph:close()
--
--      Execute 'date' command inside a shell, read the result
--      and close the popen object
--
--  ph = require('popen').popen({argv = {"/usr/bin/echo", "-n", "hello"},
--                               flags = {stdout=true, shell=false}})
--  ph:read()
--  ph:close()
--
--      Execute /usr/bin/echo with arguments '-n','hello' directly
--      without using a shell, read the result from stdout and close
--      the popen object
--
popen.popen = function(opts)
    local flags = default_flags()

    if opts == nil or type(opts) ~= 'table' then
        error("Usage: popen({argv={}[, envp={}, flags={}]")
    end

    -- Test for required arguments
    if opts["argv"] == nil then
        error("popen: argv key is missing")
    end

    -- Process flags and save updated mask
    -- to pass into engine (in case of missing
    -- flags we just use defaults).
    if opts["flags"] ~= nil then
        flags = parse_flags("popen", flags, opts["flags"])
    end
    opts["flags"] = flags

    -- We will need a number of args for speed sake
    opts["argc"] = #opts["argv"]

    -- Same applies to the environment (note though
    -- that env={} is pretty valid and env=nil
    -- as well.
    if opts["env"] ~= nil then
        opts["envc"] = #opts["env"]
    else
        opts["envc"] = -1
    end

    return popen_reify(opts)
end

return popen
