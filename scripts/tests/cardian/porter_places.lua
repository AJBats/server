-----------------------------------
-- Cardian: the places the chocobo porter rides to
-- (modules/cardian/lua/porter_places.lua, the map's pawn/porter.cpp)
--
-- The outposts' spots are asked of the game's own outpost warp, the crags'
-- read off the teleports' own table; a change to either that the library no
-- longer reads fails here.
--
-- The pawn module loads the library at init; under xi_test there is no pawn
-- module, so the test loads it itself.
-----------------------------------
require('modules/cardian/lua/porter_places')

describe('Cardian porter places', function()
    local function byKind(kind)
        local out = {}
        for _, place in ipairs(xi.cardian.porter.places()) do
            if place.kind == kind then
                out[place.key] = place
            end
        end
        return out
    end

    it('is loaded', function()
        assert(xi.cardian and xi.cardian.porter and xi.cardian.porter.places, 'modules/cardian/lua/porter_places.lua did not load')
    end)

    it('reads every region\'s outpost from the outpost warp, where it sets a player down', function()
        local outposts = byKind('outpost')
        local valkurm  = outposts[xi.region.ZULKHEIM]
        assert(valkurm ~= nil, 'Zulkheim\'s outpost is missing')
        assert(valkurm.zone == xi.zone.VALKURM_DUNES, string.format('Zulkheim\'s outpost is in zone %s', tostring(valkurm.zone)))
        assert(math.abs(valkurm.x - 148.231) < 0.01 and math.abs(valkurm.z - 93.479) < 0.01, 'Zulkheim\'s outpost is not where the warp sets a player down')
        local count = 0
        for _, place in pairs(outposts) do
            count = count + 1
            assert(place.zone ~= nil and place.x ~= nil and place.y ~= nil and place.z ~= nil, 'an outpost without its spot')
        end
        assert(count >= 17, string.format('only %d outposts', count))
        assert(outposts[xi.region.SANDORIA] == nil, 'a nation\'s own region has no outpost')
    end)

    it('reads the teleport crags from where their Teleport spells set a player down', function()
        local crags = byKind('crag')
        local holla = crags[xi.teleport.id.HOLLA]
        assert(holla ~= nil and holla.zone == xi.zone.LA_THEINE_PLATEAU, 'the Crag of Holla is not in La Theine Plateau')
        assert(crags[xi.teleport.id.DEM] ~= nil and crags[xi.teleport.id.DEM].zone == xi.zone.KONSCHTAT_HIGHLANDS, 'the Crag of Dem is not in Konschtat Highlands')
        assert(crags[xi.teleport.id.MEA] ~= nil and crags[xi.teleport.id.MEA].zone == xi.zone.TAHRONGI_CANYON, 'the Crag of Mea is not in Tahrongi Canyon')
        local count = 0
        for _ in pairs(crags) do
            count = count + 1
        end
        assert(count == 6, string.format('%d crags and telepoints, not six', count))
    end)
end)
