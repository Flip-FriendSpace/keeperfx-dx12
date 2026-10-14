-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- serialization.lua
-- Internal serialization logic for Lua<->C data.
-- Used by the engine to serialize complex Lua state; not intended for API users.

local binser = require 'external.binser'
require 'classes.Pos3d'
local serialization_version = 3
-- Check the VM/bytecode frame format before binser loads any functions.
-- The first five dump bytes include LuaJIT's version, endian and FR2 flags.
local state_prefix = 'KFXL:3:' .. string.dump(function() end):sub(1, 5) .. ':'

local registry = debug.getregistry()
for _, name in ipairs({ "Player", "Thing", "Slab", "Camera", "Room", "Map", "Pos3d" }) do
    binser.registerResource(registry[name], "metatable:" .. name)
end

local registered_cfunctions = {}
local scanned_tables = {}
local function register_cfunctions(source, prefix)
    if scanned_tables[source] then
        return
    end
    scanned_tables[source] = true
    local names = {}
    for name in pairs(source) do
        if type(name) == "string" then
            names[#names + 1] = name
        end
    end
    table.sort(names)
    for _, name in ipairs(names) do
        local value = rawget(source, name)
        local path = prefix .. name
        if type(value) == "function" and debug.getinfo(value, "S").what == "C" then
            if not registered_cfunctions[value] then
                binser.registerResource(value, path)
                registered_cfunctions[value] = true
            end
        elseif type(value) == "table" then
            register_cfunctions(value, path .. ".")
        end
    end
end
register_cfunctions(_G, "global:")
register_cfunctions(registry, "registry:")

function GetSerializedData()
    local ok, result = pcall(function()
        return state_prefix .. binser.serialize(Game, serialization_version)
    end)
    print("GetSerializedData ok: " .. tostring(ok))
    if not ok then
        error("binser failed: " .. result)
    end
    return result
end

function SetSerializedData(serialized_data)
    local ok, result = pcall(function()
        assert(serialized_data:sub(1, #state_prefix) == state_prefix, 'Incompatible Lua state format or VM')
        local values = binser.deserialize(serialized_data:sub(#state_prefix + 1))
        assert(values[2] == serialization_version, 'Incompatible Lua state version')
        assert(type(values[1]) == 'table', 'Invalid Lua Game state')
        return values[1]
    end)
    if not ok then
        error("binser load failed: " .. result)
    end
    Game = result
end
