-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- serialization.lua
-- Internal serialization logic for Lua<->C data.
-- Used by the engine to serialize complex Lua state; not intended for API users.

local binser = require 'external.binser'
local anon_functions = require 'core.anon_functions'
require 'classes.Pos3d'
local serialization_version = 3
-- Check the VM/bytecode frame format before binser loads any functions.
-- The first five dump bytes include LuaJIT's version, endian and FR2 flags.
local state_prefix = 'KFXL:3:' .. string.dump(function() end):sub(1, 5) .. ':'

local registry = debug.getregistry()
for _, name in ipairs({ "Player", "Thing", "Slab", "Camera", "Room", "Map", "Pos3d" }) do
    binser.registerResource(registry[name], "metatable:" .. name)
end

-- Registers every function (Lua or C) reachable from source by a stable string-keyed
-- path as a binser resource, so it serializes as a name instead of bytecode.
local registered_functions = {}
local scanned_tables = {}
local function register_named_functions(source, prefix)
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
        if type(value) == "function" then
            if not registered_functions[value] then
                binser.registerResource(value, path)
                registered_functions[value] = true
            end
        elseif type(value) == "table" then
            register_named_functions(value, path .. ".")
        end
    end
end
register_named_functions(_G, "global:")
register_named_functions(registry, "registry:")

-- Walks value in place, replacing every anonymous (unregistered) function it finds with
-- its anon_functions.encode() spec, and recording the change in changes so
-- revert_anon_functions can put the real functions back afterwards. Mutates the live
-- Game tree temporarily rather than cloning it, so table identity, array/hash parts and
-- metatables stay exactly what binser's own traversal expects.
local function collect_anon_functions(value, seen, changes)
    if type(value) ~= "table" or seen[value] then
        return
    end
    seen[value] = true
    for k, v in pairs(value) do
        if type(v) == "function" then
            if not registered_functions[v] then
                local spec = anon_functions.encode(v)
                if spec then
                    changes[#changes + 1] = { tbl = value, key = k, orig = v }
                    value[k] = spec
                end
            end
        elseif type(v) == "table" then
            collect_anon_functions(v, seen, changes)
        end
    end
end

local function revert_anon_functions(changes)
    for i = #changes, 1, -1 do
        changes[i].tbl[changes[i].key] = changes[i].orig
    end
end

local function restore_anon_functions(value, seen)
    if type(value) ~= "table" or seen[value] then
        return
    end
    seen[value] = true
    for k, v in pairs(value) do
        if type(v) == "table" then
            -- rawget, not v.__anon_fn: v may be a Thing/Player/etc proxy whose metatable
            -- has its own __index, which a plain index would needlessly go through.
            if rawget(v, "__anon_fn") then
                value[k] = anon_functions.decode(v)
            else
                restore_anon_functions(v, seen)
            end
        end
    end
end

function GetSerializedData()
    assert(type(Game) == "table", "Game state must be a table")
    local changes = {}
    anon_functions.begin()
    local ok, result = pcall(function()
        collect_anon_functions(Game, {}, changes)
        return state_prefix .. binser.serialize(Game, serialization_version)
    end)
    revert_anon_functions(changes)
    anon_functions.finish("saved")
    if not ok then
        error("binser failed: " .. tostring(result))
    end
    return result
end

-- allow_bytecode is only for data this engine made itself, or a file the player chose to trust.
local function restore_serialized_data(serialized_data, allow_bytecode)
    assert(serialized_data:sub(1, #state_prefix) == state_prefix, 'Incompatible Lua state format or VM')
    binser.setBytecodeAllowed(allow_bytecode)
    local values, count = binser.deserialize(serialized_data:sub(#state_prefix + 1))
    local restored = values[1]
    assert(type(restored) == "table", "Serialized game state must be a table")
    assert(count == 2 and values[2] == serialization_version,
        "Save was made with an incompatible Lua serialization version (" .. tostring(values[2]) .. ")")
    anon_functions.begin(allow_bytecode)
    restore_anon_functions(restored, {})
    anon_functions.finish("restored")
    return restored
end

function SetSerializedData(serialized_data, allow_bytecode)
    local ok, restored = pcall(restore_serialized_data, serialized_data, allow_bytecode == true)
    binser.setBytecodeAllowed(false)
    if not ok then
        error(restored, 0)
    end
    Game = restored
end
