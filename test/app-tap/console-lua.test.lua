#!/usr/bin/env tarantool
--
-- vim: ts=4 sw=4 et

local tap = require('tap')
local console = require('console')
local socket = require('socket')
local yaml = require('yaml')
local fiber = require('fiber')
local ffi = require('ffi')
local log = require('log')
local fio = require('fio')

-- Suppress console log messages
log.level(4)
local CONSOLE_SOCKET = fio.pathjoin(fio.cwd(), 'tarantool-test-console-lua.sock')
os.remove(CONSOLE_SOCKET)

local EOL = ";"

test = tap.test("console-lua")

test:plan(8)

--
-- Start console and connect to it
local server = console.listen(CONSOLE_SOCKET)
test:ok(server ~= nil, "console.listen started")

local client = socket.tcp_connect("unix/", CONSOLE_SOCKET)
local handshake = client:read{chunk = 128}
test:ok(string.match(handshake, '^Tarantool .*console') ~= nil, 'Handshake')
test:ok(client ~= nil, "connect to console")

--
-- Change output mode to Lua
client:write('\\set output lua\n')
test:is(client:read(EOL), 'true' .. EOL, "set lua output mode")

--
-- Write mulireturn statement
client:write('a={1,2,3}\n')
test:is(client:read(EOL), EOL, "declare array")

client:write('1,2,nil,a\n')
test:is(client:read(EOL), '1, 2, box.NULL, {1, 2, 3}' .. EOL, "multireturn numbers")

--
-- Try to setup empty output mode
client:write('\\set output\n')
test:is(client:read(EOL), '\"Specify output format: lua or yaml.\"' .. EOL,
        "set empty output mode")

--
-- Disconect from console
client:shutdown()
client:close()

--
-- Stop console
server:shutdown()
server:close()
fiber.sleep(0) -- workaround for gh-712: console.test.lua fails in Fedora
-- Check that admin console has been stopped
test:isnil(socket.tcp_connect("unix/", CONSOLE_SOCKET), "console.listen stopped")

test:check()
os.exit(0)
