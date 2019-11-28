fio = require('fio')
test_run = require('test_run').new()

local err, reason, exit_code

--
-- Trivial echo output
--
script="echo -n 1 2 3 4 5"
popen = fio.popen(script, "r")
popen:wait()
popen:read()
popen:close()

--
-- Test info and killing of a child process
--
script="while [ 1 ]; do sleep 10; done"
popen = fio.popen(script, "r")
popen:kill()
--
-- Killing child may be queued and depends on
-- system load, so we may get ESRCH here.
err, reason, exit_code = popen:wait()
popen:status()
info = popen:info()
info["state"]
info["flags"]
info["exit_code"]
popen:close()

--
-- Test for stdin/out stream
--
script="prompt=''; read -n 5 prompt; echo -n $prompt"
popen = fio.popen(script, "rw")
popen:write("input")
popen:read()
popen:close()
