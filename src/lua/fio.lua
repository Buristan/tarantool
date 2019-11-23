-- fio.lua (internal file)

local fio = require('fio')
local ffi = require('ffi')
local buffer = require('buffer')
local fiber = require('fiber')
local errno = require('errno')
local bit = require('bit')

ffi.cdef[[
    int umask(int mask);
    char *dirname(char *path);
    int chdir(const char *path);
]]

local const_char_ptr_t = ffi.typeof('const char *')

local internal = fio.internal
fio.internal = nil

local function sprintf(fmt, ...)
    if select('#', ...) == 0 then
        return fmt
    end
    return string.format(fmt, ...)
end

local fio_methods = {}

-- read() -> str
-- read(buf) -> len
-- read(size) -> str
-- read(buf, size) -> len
fio_methods.read = function(self, buf, size)
    local tmpbuf
    if (not ffi.istype(const_char_ptr_t, buf) and buf == nil) or
        (ffi.istype(const_char_ptr_t, buf) and size == nil) then
        local st, err = self:stat()
        if st == nil then
            return nil, err
        end
        size = st.size
    end
    if not ffi.istype(const_char_ptr_t, buf) then
        size = buf or size
        tmpbuf = buffer.ibuf()
        buf = tmpbuf:reserve(size)
    end
    local res, err = internal.read(self.fh, buf, size)
    if res == nil then
        if tmpbuf ~= nil then
            tmpbuf:recycle()
        end
        return nil, err
    end
    if tmpbuf ~= nil then
        tmpbuf:alloc(res)
        res = ffi.string(tmpbuf.rpos, tmpbuf:size())
        tmpbuf:recycle()
    end
    return res
end

-- write(str)
-- write(buf, len)
fio_methods.write = function(self, data, len)
    if not ffi.istype(const_char_ptr_t, data) then
        data = tostring(data)
        len = #data
    end
    local res, err = internal.write(self.fh, data, len)
    if err ~= nil then
        return false, err
    end
    return res >= 0
end

-- pwrite(str, offset)
-- pwrite(buf, len, offset)
fio_methods.pwrite = function(self, data, len, offset)
    if not ffi.istype(const_char_ptr_t, data) then
        data = tostring(data)
        offset = len
        len = #data
    end
    local res, err = internal.pwrite(self.fh, data, len, offset)
    if err ~= nil then
        return false, err
    end
    return res >= 0
end

-- pread(size, offset) -> str
-- pread(buf, size, offset) -> len
fio_methods.pread = function(self, buf, size, offset)
    local tmpbuf
    if not ffi.istype(const_char_ptr_t, buf) then
        offset = size
        size = buf
        tmpbuf = buffer.IBUF_SHARED
        tmpbuf:reset()
        buf = tmpbuf:reserve(size)
    end
    local res, err = internal.pread(self.fh, buf, size, offset)
    if res == nil then
        if tmpbuf ~= nil then
            tmpbuf:recycle()
        end
        return nil, err
    end
    if tmpbuf ~= nil then
        tmpbuf:alloc(res)
        res = ffi.string(tmpbuf.rpos, tmpbuf:size())
        tmpbuf:recycle()
    end
    return res
end

fio_methods.truncate = function(self, length)
    if length == nil then
        length = 0
    end
    return internal.ftruncate(self.fh, length)
end

fio_methods.seek = function(self, offset, whence)
    if whence == nil then
        whence = 'SEEK_SET'
    end
    if type(whence) == 'string' then
        if fio.c.seek[whence] == nil then
            error(sprintf("fio.seek(): unknown whence: %s", whence))
        end
        whence = fio.c.seek[whence]
    else
        whence = tonumber(whence)
    end

    local res = internal.lseek(self.fh, tonumber(offset), whence)
    return tonumber(res)
end

fio_methods.close = function(self)
    local res, err = internal.close(self.fh)
    self.fh = -1
    if err ~= nil then
        return false, err
    end
    return res
end

fio_methods.fsync = function(self)
    return internal.fsync(self.fh)
end

fio_methods.fdatasync = function(self)
    return internal.fdatasync(self.fh)
end

fio_methods.stat = function(self)
    return internal.fstat(self.fh)
end

local fio_mt = { __index = fio_methods }

fio.open = function(path, flags, mode)
    local iflag = 0
    local imode = 0
    if type(path) ~= 'string' then
        error("Usage: fio.open(path[, flags[, mode]])")
    end
    if type(flags) ~= 'table' then
        flags = { flags }
    end
    if type(mode) ~= 'table' then
        mode = { mode or (bit.band(0x1FF, fio.umask())) }
    end


    for _, flag in pairs(flags) do
        if type(flag) == 'number' then
            iflag = bit.bor(iflag, flag)
        else
            if fio.c.flag[ flag ] == nil then
                error(sprintf("fio.open(): unknown flag: %s", flag))
            end
            iflag = bit.bor(iflag, fio.c.flag[ flag ])
        end
    end

    for _, m in pairs(mode) do
        if type(m) == 'string' then
            if fio.c.mode[m] == nil then
                error(sprintf("fio.open(): unknown mode: %s", m))
            end
            imode = bit.bor(imode, fio.c.mode[m])
        else
            imode = bit.bor(imode, tonumber(m))
        end
    end

    local fh, err = internal.open(tostring(path), iflag, imode)
    if err ~= nil then
        return nil, err
    end

    fh = { fh = fh }
    setmetatable(fh, fio_mt)
    return fh
end

fio.pathjoin = function(...)
    local i, path = 1, nil

    local len = select('#', ...)
    while i <= len do
        local sp = select(i, ...)
        if sp == nil then
            error("fio.pathjoin(): undefined path part "..i)
        end

        sp = tostring(sp)
        if sp ~= '' then
            path = sp
            break
        else
            i = i + 1
        end
    end

    if path == nil then
        return '.'
    end

    i = i + 1
    while i <= len do
        local sp = select(i, ...)
        if sp == nil then
            error("fio.pathjoin(): undefined path part "..i)
        end

        sp = tostring(sp)
        if sp ~= '' then
            path = path .. '/' .. sp
        end

        i = i + 1
    end

    path = path:gsub('/+', '/')
    if path ~= '/' then
        path = path:gsub('/$', '')
    end

    return path
end

fio.basename = function(path, suffix)
    if type(path) ~= 'string' then
        error("Usage: fio.basename(path[, suffix])")
    end

    path = tostring(path)
    path = string.gsub(path, '.*/', '')

    if suffix ~= nil then
        suffix = tostring(suffix)
        if #suffix > 0 then
            suffix = string.gsub(suffix, '(.)', '[%1]')
            path = string.gsub(path, suffix, '')
        end
    end

    return path
end

fio.dirname = function(path)
    if type(path) ~= 'string' then
        error("Usage: fio.dirname(path)")
    end
    -- Can't just cast path to char * - on Linux dirname modifies
    -- its argument.
    local bsize = #path + 1
    local cpath = buffer.static_alloc('char', bsize)
    ffi.copy(cpath, ffi.cast('const char *', path), bsize)
    return ffi.string(ffi.C.dirname(cpath))
end

fio.umask = function(umask)

    if umask == nil then
        local old = ffi.C.umask(0)
        ffi.C.umask(old)
        return old
    end

    umask = tonumber(umask)

    return ffi.C.umask(tonumber(umask))

end

fio.abspath = function(path)
    -- following established conventions of fio module:
    -- letting nil through and converting path to string
    if path == nil then
        error("Usage: fio.abspath(path)")
    end
    path = path
    local joined_path = ''
    local path_tab = {}
    if string.sub(path, 1, 1) == '/' then
        joined_path = path
    else
        joined_path = fio.pathjoin(fio.cwd(), path)
    end
    for sp in string.gmatch(joined_path, '[^/]+') do
        if sp == '..' then
            table.remove(path_tab)
        elseif sp ~= '.' then
            table.insert(path_tab, sp)
        end
    end
    return '/' .. table.concat(path_tab, '/')
end

fio.chdir = function(path)
    if type(path)~='string' then
        error("Usage: fio.chdir(path)")
    end
    return ffi.C.chdir(path) == 0
end

fio.listdir = function(path)
    if type(path) ~= 'string' then
        error("Usage: fio.listdir(path)")
    end
    local str, err = internal.listdir(path)
    if err ~= nil then
        return nil, string.format("can't listdir %s: %s", path, err)
    end
    local t = {}
    if str == "" then
        return t
    end
    local names = string.split(str, "\n")
    for i, name in ipairs(names) do
        table.insert(t, name)
    end
    return t
end

fio.mktree = function(path, mode)
    if type(path) ~= "string" then
        error("Usage: fio.mktree(path[, mode])")
    end
    path = fio.abspath(path)

    local path = string.gsub(path, '^/', '')
    local dirs = string.split(path, "/")

    local current_dir = "/"
    for i, dir in ipairs(dirs) do
        current_dir = fio.pathjoin(current_dir, dir)
        local stat = fio.stat(current_dir)
        if stat == nil then
            local st, err = fio.mkdir(current_dir, mode)
            if err ~= nil  then
                return false, string.format("Error creating directory %s: %s",
                    current_dir, tostring(err))
            end
        elseif not stat:is_dir() then
            return false, string.format("Error creating directory %s: %s",
                current_dir, errno.strerror(errno.EEXIST))
        end
    end
    return true
end

fio.rmtree = function(path)
    if type(path) ~= 'string' then
        error("Usage: fio.rmtree(path)")
    end
    local status, err
    path = fio.abspath(path)
    local ls, err = fio.listdir(path)
    if err ~= nil then
        return nil, err
    end
    for i, f in ipairs(ls) do
        local tmppath = fio.pathjoin(path, f)
        local st = fio.lstat(tmppath)
        if st then
            if st:is_dir() then
                st, err = fio.rmtree(tmppath)
            else
                st, err = fio.unlink(tmppath)
            end
            if err ~= nil  then
                return nil, err
            end
        end
    end
    status, err = fio.rmdir(path)
    if err ~= nil then
        return false, string.format("failed to remove %s: %s", path, err)
    end
    return true
end

fio.copyfile = function(from, to)
    if type(from) ~= 'string' or type(to) ~= 'string' then
        error('Usage: fio.copyfile(from, to)')
    end
    local st = fio.stat(to)
    if st and st:is_dir() then
        to = fio.pathjoin(to, fio.basename(from))
    end
    local _, err = internal.copyfile(from, to)
    if err ~= nil then
        return false, string.format("failed to copy %s to %s: %s", from, to, err)
    end
    return true
end

fio.copytree = function(from, to)
    if type(from) ~= 'string' or type(to) ~= 'string' then
        error('Usage: fio.copytree(from, to)')
    end
    local status, reason
    local st = fio.stat(from)
    if not st then
        return false, string.format("Directory %s does not exist", from)
    end
    if not st:is_dir() then
        return false, errno.strerror(errno.ENOTDIR)
    end
    local ls, err = fio.listdir(from)
    if err ~= nil then
        return false, err
    end

    -- create tree of destination
    status, reason = fio.mktree(to)
    if reason ~= nil then
        return false, reason
    end
    for i, f in ipairs(ls) do
        local ffrom = fio.pathjoin(from, f)
        local fto = fio.pathjoin(to, f)
        local st = fio.lstat(ffrom)
        if st and st:is_dir() then
            status, reason = fio.copytree(ffrom, fto)
            if reason ~= nil then
                return false, reason
            end
        end
        if st:is_reg() then
            status, reason = fio.copyfile(ffrom, fto)
            if reason ~= nil then
                return false, reason
            end
        end
        if st:is_link() then
            local link_to, reason = fio.readlink(ffrom)
            if reason ~= nil then
                return false, reason
            end
            status, reason = fio.symlink(link_to, fto)
            if reason ~= nil then
                return false, "can't create symlink in place of existing file "..fto
            end
        end
    end
    return true
end

local function check_time(time, name)
    if time ~= nil and type(time) ~= 'number' then
        error('fio.utime: ' .. name .. ' should be a number', 2)
    end
end

fio.utime = function(path, atime, mtime)
    if type(path) ~= 'string' then
        error('Usage: fio.utime(filepath[, atime[, mtime]])')
    end

    check_time(atime, 'atime')
    check_time(mtime, 'mtime')

    local current_time = fiber.time()
    atime = atime or current_time
    mtime = mtime or atime

    return internal.utime(path, atime, mtime)
end

fio.path = {}
fio.path.is_file = function(filename)
    local fs = fio.stat(filename)
    return fs ~= nil and fs:is_reg() or false
end

fio.path.is_link = function(filename)
    local fs = fio.lstat(filename)
    return fs ~= nil and fs:is_link() or false
end

fio.path.is_dir = function(filename)
    local fs = fio.stat(filename)
    return fs ~= nil and fs:is_dir() or false
end

fio.path.exists = function(filename)
    return fio.stat(filename) ~= nil
end

fio.path.lexists = function(filename)
    return fio.lstat(filename) ~= nil
end

local popen_methods = { }

--
-- Make sure the bits are matching POPEN_FLAG_x
-- flags in popen header. This is api but internal
-- one, we should not make it visible to users.
local popen_flag_bits = {
    STDIN           = 0,
    STDOUT          = 1,
    STDERR          = 2,

    STDIN_DEVNULL   = 3,
    STDOUT_DEVNULL  = 4,
    STDERR_DEVNULL  = 5,

    STDIN_CLOSE     = 6,
    STDOUT_CLOSE    = 7,
    STDERR_CLOSE    = 8,

    --
    -- bits 9,10,11 are reserved
    -- for future use



    SHELL           = 12,
    SETSID          = 13,
    CLOSE_FDS       = 14,
    RESTORE_SIGNALS = 15,
}

local popen_flags = {
    NONE            = 0,
    STDIN           = bit.lshift(1, popen_flag_bits.STDIN),
    STDOUT          = bit.lshift(1, popen_flag_bits.STDOUT),
    STDERR          = bit.lshift(1, popen_flag_bits.STDERR),

    STDIN_DEVNULL   = bit.lshift(1, popen_flag_bits.STDIN_DEVNULL),
    STDOUT_DEVNULL  = bit.lshift(1, popen_flag_bits.STDOUT_DEVNULL),
    STDERR_DEVNULL  = bit.lshift(1, popen_flag_bits.STDERR_DEVNULL),

    STDIN_CLOSE     = bit.lshift(1, popen_flag_bits.STDIN_CLOSE),
    STDOUT_CLOSE    = bit.lshift(1, popen_flag_bits.STDOUT_CLOSE),
    STDERR_CLOSE    = bit.lshift(1, popen_flag_bits.STDERR_CLOSE),

    SHELL           = bit.lshift(1, popen_flag_bits.SHELL),
    SETSID          = bit.lshift(1, popen_flag_bits.SETSID),
    CLOSE_FDS       = bit.lshift(1, popen_flag_bits.CLOSE_FDS),
    RESTORE_SIGNALS = bit.lshift(1, popen_flag_bits.RESTORE_SIGNALS),
}

--
-- Get default flags for popen
local function popen_default_flags()
    local flags = popen_flags.NONE

    -- default flags: close everything and use shell
    flags = bit.bor(flags, popen_flags.STDIN_CLOSE)
    flags = bit.bor(flags, popen_flags.STDOUT_CLOSE)
    flags = bit.bor(flags, popen_flags.STDERR_CLOSE)
    flags = bit.bor(flags, popen_flags.SHELL)
    flags = bit.bor(flags, popen_flags.SETSID)
    flags = bit.bor(flags, popen_flags.CLOSE_FDS)
    flags = bit.bor(flags, popen_flags.RESTORE_SIGNALS)

    return flags
end

--
-- Close a popen object and release all resources.
-- In case if there is a running child process
-- it will be killed.
--
-- Returns @ret = true if popen handle closed, and
-- @ret = false, @err ~= nil on error.
popen_methods.close = function(self)
    local ret, err = internal.popen_delete(self.popen_handle)
    if err ~= nil then
	    return false, err
    end
    self.popen_handle = nil
    return true
end

--
-- Kill a child process
--
-- Returns @ret = true on success,
-- @ret = false, @err ~= nil on error.
popen_methods.kill = function(self)
    return internal.popen_kill(self.popen_handle)
end

--
-- Terminate a child process
--
-- Returns @ret = true on success,
-- @ret = false, @err ~= nil on error.
popen_methods.terminate = function(self)
    return internal.popen_term(self.popen_handle)
end

--
-- Fetch a child process state
--
-- Returns @err = nil, @state = (1 - if exited,
-- 2 - if killed by a signal) and @exit_code ~= nil,
-- otherwise @err ~= nil.
--
-- FIXME: Should the @state be a named constant?
popen_methods.state = function(self)
    return internal.popen_state(self.popen_handle)
end

--
-- Wait until a child process get exited.
--
-- Returns the same as popen_methods.status.
popen_methods.wait = function(self)
    local err, state, code
    while true do
        err, state, code = internal.popen_state(self.popen_handle)
        if err or state then
            break
        end
        fiber.sleep(self.wait_secs)
    end
    return err, state, code
end

--
-- A map for popen option keys into tfn ('true|false|nil') values
-- where bits are just set without additional manipulations.
--
-- For example stdin=true means to open stdin end for write,
-- stdin=false to close the end, finally stdin[=nil] means to
-- provide /dev/null into a child peer.
local popen_opts_tfn = {
    stdin = {
        popen_flags.STDIN,
        popen_flags.STDIN_CLOSE,
        popen_flags.STDIN_DEVNULL,
    },
    stdout = {
        popen_flags.STDOUT,
        popen_flags.STDOUT_CLOSE,
        popen_flags.STDOUT_DEVNULL,
    },
    stderr = {
        popen_flags.STDERR,
        popen_flags.STDERR_CLOSE,
        popen_flags.STDERR_DEVNULL,
    },
}

--
-- A map for popen option keys into tf ('true|false') values
-- where bits are set on 'true' and clear on 'false'.
local popen_opts_tf = {
    shell = {
        popen_flags.SHELL,
    },
    close_fds = {
        popen_flags.CLOSE_FDS
    },
    restore_signals = {
        popen_flags.RESTORE_SIGNALS
    },
    start_new_session = {
        popen_flags.SETSID
    },
}

--
-- Parses flags options from popen_opts_tfn and
-- popen_opts_tf tables.
local function parse_popen_flags(epfx, flags, opts)
    if opts == nil then
        return flags
    end
    for k,v in pairs(opts) do
        if popen_opts_tfn[k] == nil then
            if popen_opts_tf[k] == nil then
                error(string.format("%s: Unknown key %s", epfx, k))
            end
            if v == true then
                flags = bit.bor(flags, popen_opts_tf[k][1])
            elseif v == false then
                flags = bit.band(flags, bit.bnot(popen_opts_tf[k][1]))
            else
                error(string.format("%s: Unknown value %s", epfx, v))
            end
        else
            if v == true then
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][2]))
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][3]))
                flags = bit.bor(flags, popen_opts_tfn[k][1])
            elseif v == false then
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][1]))
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][3]))
                flags = bit.bor(flags, popen_opts_tfn[k][2])
            elseif v == "devnull" then
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][1]))
                flags = bit.band(flags, bit.bnot(popen_opts_tfn[k][2]))
                flags = bit.bor(flags, popen_opts_tfn[k][3])
            else
                error(string.format("%s: Unknown value %s", epfx, v))
            end
        end
    end
    return flags
end

--
-- popen:read2 - read stream of a child with options
-- @opts:       options table
--
-- The options should have the following keys
--
-- @flags:      stdout=true or stderr=true
-- @buf:        const_char_ptr_t buffer
-- @size:       size of the buffer
-- @timeout:    read timeout in microsecond
--
-- Returns @res = bytes, err = nil in case if read processed
-- without errors, @res = nil, @err = nil if timeout elapsed
-- and no data appeared on the peer, @res = nil, @err ~= nil
-- otherwise.
popen_methods.read2 = function(self, opts)
    --
    -- We can reuse parse_popen_flags since only
    -- a few flags we're interesed in -- stdout/stderr
    local flags = parse_popen_flags("fio.popen:read2",
                                    popen_flags.NONE,
                                    opts['flags'])
    local timeout = -1
    if opts['buf'] == nil then
        error("fio.popen:read2 {'buf'} key is missed")
    elseif opts['size'] == nil then
        error("fio.popen:read2 {'size'} key is missed")
    elseif opts['timeout'] ~= nil then
        timeout = tonumber(opts['timeout'])
    end

    return internal.popen_read(self.popen_handle, opts['buf'],
                               tonumber(opts['size']),
                               timeout, flags)
end

-- popen:write2 - write data to a child with options
-- @opts:       options table
--
-- The options should have the following keys
--
-- @flags:      stdin=true
-- @buf:        const_char_ptr_t buffer
-- @size:       size of the buffer
--
-- Returns @res = bytes, err = nil in case if write processed
-- without errors, or @res = nil, @err ~= nil otherwise.
popen_methods.write2 = function(self, opts)
    local flags = parse_popen_flags("fio.popen:write2",
                                    popen_flags.NONE,
                                    opts['flags'])
    if opts['buf'] == nil then
        error("fio.popen:write2 {'buf'} key is missed")
    elseif opts['size'] == nil then
        error("fio.popen:write2 {'size'} key is missed")
    end
    return internal.popen_write(self.popen_handle, opts['buf'],
                                tonumber(opts['size']), flags)
end

--
-- Write data to stdin of a child process
-- @buf:        a buffer to write
-- @size:       size of the buffer
--
-- Both parameters are optional and some
-- string could be passed instead
--
-- write(str)
-- write(buf, len)
--
-- Returns @res = bytes, @err = nil in case of success,
-- @res = , @err ~= nil in case of error.
popen_methods.write = function(self, buf, size)
    if not ffi.istype(const_char_ptr_t, buf) then
        buf = tostring(buf)
        size = #buf
    end
    return self:write2({ buf = buf, size = size,
                       flags = { stdin = true }})
end

--
-- popen:read - read the output stream in blocked mode
-- @buf:        a buffer to read to, optional
-- @size:       size of the buffer, optional
-- @opts:       options table, optional
--
-- The options may have the following keys:
--
-- @flags:      stdout=true or stderr=true,
--              to specify which stream to read;
--              by default stdout is used
--
-- @timeout:    read timeout in microcesonds, when
--              specified the read will be interrupted
--              after specified time elapced, to make
--              nonblocking read; by default the read
--              is blocking operation
--
-- The following variants are accepted as well
--
--  read()          -> str
--  read(buf)       -> len
--  read(size)      -> str
--  read(buf, size) -> len
--
-- FIXME: Should we merged it with plain fio.write()?
popen_methods.read = function(self, buf, size, opts)
    local tmpbuf

    -- read() or read(buf)
    if (not ffi.istype(const_char_ptr_t, buf) and buf == nil) or
        (ffi.istype(const_char_ptr_t, buf) and size == nil) then
        -- read @self.read_buf_size bytes at once by default
        size = self.read_buf_size
    end

    -- read(size)
    if not ffi.istype(const_char_ptr_t, buf) then
        size = buf or size
        tmpbuf = buffer.ibuf()
        buf = tmpbuf:reserve(size)
    end

    if opts == nil then
        opts = {
            timeout = -1,
            flags = { stdout = true },
        }
    end

    local res, err = self:read2({ buf = buf, size = size,
                                timeout = opts['timeout'],
                                flags = opts['flags'] })

    if res == nil then
        if tmpbuf ~= nil then
            tmpbuf:recycle()
        end
        return nil, err
    end

    if tmpbuf ~= nil then
        tmpbuf:alloc(res)
        res = ffi.string(tmpbuf.rpos, tmpbuf:size())
        tmpbuf:recycle()
    end

    return res
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
-- @flags:      flags associated (popen_flags bitmap)
-- @stdout:     parent peer for stdout, -1 if closed
-- @stderr:     parent peer for stderr, -1 if closed
-- @stdin:      parent peer for stdin, -1 if closed
-- @state:      alive | exited | signaled | unknown
-- @exit_code:  exit code of a child process if been
--              exited or killed by a signal
--
-- The child should never be in "unknown" state, reserved
-- for unexpected errors.
--
popen_methods.info = function(self)
    return internal.popen_info(self.popen_handle)
end

local popen_mt = { __index = popen_methods }

--
-- Create a new popen object from options
local function popen_new(opts)
    local handle, err = internal.popen_new(opts)
    if err ~= nil then
        return nil, err
    end

    local popen_handle = {
        -- a handle itself for future use
        popen_handle = handle,

        -- sleeping period for the @wait method
        wait_secs = 1,

        -- size of a read buffer to allocate
        -- in case of implicit read
        read_buf_size = 512,
    }

    setmetatable(popen_handle, popen_mt)
    return popen_handle
end

--
-- Parse "mode" string to flags
local function parse_popen_mode(epfx, flags, mode)
    if mode == nil then
        return flags
    end
    for i = 1, #mode do
        local c = mode:sub(i, i)
        if c == 'r' then
            flags = bit.band(flags, bit.bnot(popen_flags.STDOUT_CLOSE))
            flags = bit.bor(flags, popen_flags.STDOUT)
            flags = bit.band(flags, bit.bnot(popen_flags.STDERR_CLOSE))
            flags = bit.bor(flags, popen_flags.STDERR)
        elseif c == 'w' then
            flags = bit.band(flags, bit.bnot(popen_flags.STDIN_CLOSE))
            flags = bit.bor(flags, popen_flags.STDIN)
        else
            error(string.format("%s: Unknown mode %s", epfx, c))
        end
    end
    return flags
end

--
-- fio.popen_posix - create a new child process and execute a command inside
-- @command:    a command to run
-- @mode:       r - to grab child's stdout for read [*]
--              (stderr kept opened as well for 2>&1 redirection)
--              w - to obtain stdin for write
--
-- [*] are default values
--
-- Note: Since there are two options only the following parameters
-- are implied (see popen_default_flags): all fds except stdin/out/err
-- are closed, signals are restored to default handlers, the command
-- is executed in a new session.
--
-- Examples:
--
--  popen = require('fio').popen_posix("date", "r")
--      runs date as "sh -c date" to read the output,
--      closing all file descriptors except stdout/err
--      inside a child process
--
fio.popen_posix = function(command, mode)
    local flags = popen_default_flags()

    if type(command) ~= 'string' then
        error("Usage: fio.popen(command[, rw])")
    end

    -- Mode gives simplified flags
    flags = parse_popen_mode("fio.popen", flags, mode)

    local opts = {
        argv    = { command },
        argc    = 1,
        flags   = flags,
        envc    = -1,
    }

    return popen_new(opts)
end

-- fio.popen - execute a child program in a new process
-- @opt:    table of options
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
--      stdout=true     to write into STDOUT_FILENO of a child process
--      stdout=false    to close STDOUT_FILENO inside a child process [*]
--      stdin="devnull" a child will get /dev/null as STDOUT_FILENO
--
--      stderr=true     to read STDERR_FILENO from a child process
--      stderr=false    to close STDERR_FILENO inside a child process [*]
--      stdin="devnull" a child will get /dev/null as STDERR_FILENO
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
--  popen = require('fio').popen({argv = {"date"}, flags = {stdout=true}})
--  popen:read()
--  popen:close()
--
--      Execute 'date' command inside a shell, read the result
--      and close the popen object
--
--  popen = require('fio').popen({argv = {"/usr/bin/echo", "-n", "hello"},
--                               flags = {stdout=true, shell=false}})
--  popen:read()
--  popen:close()
--
--      Execute /usr/bin/echo with arguments '-n','hello' directly
--      without using a shell, read the result from stdout and close
--      the popen object
--
fio.popen = function(opts)
    local flags = popen_default_flags()

    if opts == nil or type(opts) ~= 'table' then
        error("Usage: fio.run({argv={},envp={},flags={}")
    end

    -- Test for required arguments
    if opts["argv"] == nil then
        error("fio.run: argv key is missing")
    end

    -- Process flags and save updated mask
    -- to pass into engine (in case of missing
    -- flags we just use defaults).
    if opts["flags"] ~= nil then
        flags = parse_popen_flags("fio.run", flags, opts["flags"])
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

    return popen_new(opts)
end

return fio
