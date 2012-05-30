-- Import frlib.
package.path = "frlib/?.lua;" .. package.path
local fr = require "flexrender"

-- Handy aliases.
local vec2 = fr.vec2
local vec3 = fr.vec3

network {
    workers = {
        "129.65.51.112",
        "129.65.51.113",
        "129.65.51.114",
        "129.65.51.115"
    },
    runaway = 5 -- percent
}

output {
    size = vec2(640, 480),
    name = "field",
    buffers = {
        -- none
    }
}

render {
    antialiasing = 1,
    samples = 1,
    bounces = 3,
    threshold = 0.0001,
    min = vec3(-75, -5, -75),
    max = vec3(75, 25, 75),
    --min = vec3(-10, -10, -10),
    --max = vec3(10, 10, 10),
    --min = vec3(-5, -5, -5),
    --max = vec3(15, 15, 15)
}
