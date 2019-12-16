-- Test popen engine
--
-- vim: ts=4 sw=4 et

buffer = require('buffer')
popen = require('popen')
ffi = require('ffi')

test_run = require('test_run').new()

buf = buffer.ibuf()

--
-- Trivial echo output
--
script = "echo -n 1 2 3 4 5"
ph = popen.posix(script, "r")
ph:wait()   -- wait echo to finish
ph:read()   -- read the output
ph:close()  -- release the popen

--
-- Test info and force killing of a child process
--
script = "while [ 1 ]; do sleep 10; done"
ph = popen.posix(script, "r")
ph:kill()
--
-- Killing child may be queued and depends on
-- system load, so we may get ESRCH here.
err, reason, exit_code = ph:wait()
ph:state()
info = ph:info()
info["command"]
info["state"]
info["flags"]
info["exit_code"]
ph:close()

--
-- Test info and soft killing of a child process
--
script = "while [ 1 ]; do sleep 10; done"
ph = popen.posix(script, "r")
ph:terminate()
--
-- Killing child may be queued and depends on
-- system load, so we may get ESRCH here.
err, reason, exit_code = ph:wait()
ph:state()
info = ph:info()
info["command"]
info["state"]
info["flags"]
info["exit_code"]
ph:close()

--
-- Test for stdin/out stream
--
script="prompt=''; read -n 5 prompt; echo -n $prompt"
ph = popen.posix(script, "rw")
ph:write("input")
ph:read()
ph:close()

--
-- Test reading stderr (simply redirect stdout to stderr)
--
script = "echo -n 1 2 3 4 5 1>&2"
ph = popen.posix(script, "rw")
ph:wait()
size = 128
dst = buf:reserve(size)
res, err = ph:read2({buf = dst, size = size, nil, flags = {stderr = true}})
res = buf:alloc(res)
ffi.string(buf.rpos, buf:size())
buf:recycle()
ph:close()

--
-- Test timeouts: just wait for 0.1 second
-- to elapse, then write data and re-read
-- for sure.
--
script = "prompt=''; read -n 5 prompt && echo -n $prompt;"
ph = popen.posix(script, "rw")
ph:read(nil, 0.1)
ph:write("input")
ph:read()
ph:close()
