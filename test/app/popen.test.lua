fio = require('fio')
buffer = require('buffer')
ffi = require('ffi')

test_run = require('test_run').new()

buf = buffer.ibuf()

--
-- Trivial echo output
--
script = "echo -n 1 2 3 4 5"
popen = fio.popen_posix(script, "r")
popen:wait()    -- wait echo to finish
popen:read()    -- read the output
popen:close()   -- release the popen

--
-- Test info and force killing of a child process
--
script = "while [ 1 ]; do sleep 10; done"
popen = fio.popen_posix(script, "r")
popen:kill()
--
-- Killing child may be queued and depends on
-- system load, so we may get ESRCH here.
err, reason, exit_code = popen:wait()
popen:state()
info = popen:info()
info["command"]
info["state"]
info["flags"]
info["exit_code"]
popen:close()

--
-- Test info and soft killing of a child process
--
script = "while [ 1 ]; do sleep 10; done"
popen = fio.popen_posix(script, "r")
popen:terminate()
--
-- Killing child may be queued and depends on
-- system load, so we may get ESRCH here.
err, reason, exit_code = popen:wait()
popen:state()
info = popen:info()
info["command"]
info["state"]
info["flags"]
info["exit_code"]
popen:close()


--
-- Test for stdin/out stream
--
script="prompt=''; read -n 5 prompt; echo -n $prompt"
popen = fio.popen_posix(script, "rw")
popen:write("input")
popen:read()
popen:close()

--
-- Test reading stderr (simply redirect stdout to stderr)
--
script = "echo -n 1 2 3 4 5 1>&2"
popen = fio.popen_posix(script, "rw")
popen:wait()
size = 128
dst = buf:reserve(size)
res, err = popen:read2({buf = dst, size = size, nil, flags = {stderr = true}})
res = buf:alloc(res)
ffi.string(buf.rpos, buf:size())
buf:recycle()
popen:close()

--
-- Test timeouts: just wait for 10 microseconds
-- to elapse, then write data and re-read for sure.
--
script = "prompt=''; read -n 5 prompt && echo -n $prompt;"
popen = fio.popen_posix(script, "rw")
popen:read(nil, nil, {timeout = 10, flags = {stdout = true}})
popen:write("input")
popen:read()
popen:close()
