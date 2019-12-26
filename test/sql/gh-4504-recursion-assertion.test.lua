test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

--
-- Start transaction prior to SELECT query
-- to make sure GC is called only after
-- the whole statement is executed
--
box.schema.func.create('_C', {                     \
    language = 'LUA',                              \
    returns = 'unsigned',                          \
    body = [[                                      \
        function (i)                               \
            local m = box.space._space:select()    \
            return 0                               \
        end                                        \
    ]],                                            \
    param_list = {'integer'},                      \
    exports = {'LUA', 'SQL'},                      \
    is_sandboxed = false,                          \
    is_deterministic = true})

box.execute([[WITH RECURSIVE x AS (SELECT 0 AS q, 1 UNION ALL \
    SELECT q+1, _c(q) FROM x WHERE q < 1) SELECT * FROM x;]])

box.schema.func.drop("_C")
