#!/usr/bin/env tarantool
os = require('os')

box.cfg{
    listen = os.getenv("LISTEN"),
    read_only = false
}

require('console').listen(os.getenv('ADMIN'))
