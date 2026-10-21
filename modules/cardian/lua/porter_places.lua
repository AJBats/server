-----------------------------------
-- Cardian: the places the chocobo porter rides to (the map's
-- src/map/pawn/porter.cpp; ROADMAP T, "The chocobo porter")
--
-- Every spot is the game's own: a conquest outpost's is where the outpost
-- warp sets a player down (xi.teleport.toOutpost, scripts/globals/
-- teleports.lua, whose table of them is file-local there: it is asked, its
-- player a stand-in that keeps where it was set down), and a teleport
-- crag's is where its Teleport spell sets a player down
-- (xi.teleport.destination). The cities are the map's own, from the trek's
-- world graph.
--
--   xi.cardian.porter.places()
--     -> { { kind = 'outpost' | 'crag', key, zone, x, y, z } ... }: key an
--        outpost's region (xi.region), a crag's teleport (xi.teleport.id).
--
-- A library, not a module: it overrides nothing, so the pawn module loads it
-- at init (pawn::linkapi::loadLibraries), and xi_test requires it from the
-- test.
-----------------------------------
xi = xi or {}
xi.cardian = xi.cardian or {}
xi.cardian.porter = {}

-- The teleport crags, by their teleport: the three crags of the Promyvion
-- and the three telepoints
local crags = { 'HOLLA', 'DEM', 'MEA', 'ALTEP', 'YHOAT', 'VAHZL' }

-- Where the outpost warp of this region sets a player down, or nil where it
-- has none: asked of the warp itself, its player a stand-in
local function outpostSpot(region)
    local spot     = nil
    local standIn  = { setPos = function(_, x, y, z, rotation, zone) spot = { x = x, y = y, z = z, zone = zone } end }
    local ok       = pcall(xi.teleport.toOutpost, standIn, region)
    if not ok or spot == nil or spot.zone == nil then
        return nil
    end
    return spot
end

xi.cardian.porter.places = function()
    local out = {}
    local regions = {}
    for _, region in pairs(xi.region) do
        table.insert(regions, region)
    end
    table.sort(regions)
    for _, region in ipairs(regions) do
        local spot = outpostSpot(region)
        if spot ~= nil then
            table.insert(out, { kind = 'outpost', key = region, zone = spot.zone, x = spot.x, y = spot.y, z = spot.z })
        end
    end
    for _, name in ipairs(crags) do
        local id   = xi.teleport.id[name]
        local dest = id ~= nil and xi.teleport.destination[id] or nil
        if dest ~= nil then
            table.insert(out, { kind = 'crag', key = id, zone = dest[5], x = dest[1], y = dest[2], z = dest[3] })
        end
    end
    return out
end
