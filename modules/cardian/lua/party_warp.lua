-----------------------------------
-- Cardian: the party warps with the player
--
-- A warp the player buys -- from his nation's outpost teleporter in town (to
-- an outpost), or from an outpost vendor (home to his nation's capital) --
-- takes him alone. With cardians beside him, the question of the party is
-- part of the purchase: picking the warp in the NPC's menu buys nothing yet,
-- and the server asks him in the Cardian menu whether to take the whole party
-- (the Link's OFFER, src/map/pawn/offers.h). Yes buys the warp then, through
-- upstream's own handler -- its checks, and its fee in gil or conquest points
-- -- and he and every cardian still beside him warp to his destination. No,
-- cancel, no answer in time, his leaving the zone or signing out, or another
-- warp bought meanwhile, call the whole warp off: nothing is charged and
-- nobody moves. Only he pays, as ever.
--
-- With nobody beside him, or no addon bound to ask through, the warp is
-- upstream's, unchanged. Scope: cardians in his party, in his zone, standing
-- (not KO'd); owned and wild alike.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_party_warp')

-- How long the question stands, in seconds of the simulation's time
local patience = 30

-- The two handlers a purchase is finished by
local teleporter = 1 -- xi.conquest.teleporterOnEventFinish: town to an outpost
local vendor     = 2 -- xi.conquest.vendorOnEventFinish: an outpost home

-- Upstream's own finish handlers, as each override is handed them (super),
-- kept to buy a held warp with on a yes
local upstream = {}

-- The options of each handler that buy a warp: a copy of upstream's option
-- numbers in scripts/globals/conquest.lua (the teleporter's 5-23 for gil and
-- 1029-1047 for conquest points, the vendor's 2 for gil and 6 for conquest
-- points), compared with upstream's at every merge from base (CLAUDE.md)
local function buysWarp(handler, option, csid, teleporterEvent)
    if handler == teleporter then
        return csid == teleporterEvent and ((option >= 5 and option <= 23) or (option >= 1029 and option <= 1047))
    end

    return option == 2 or option == 6
end

-- His nation's capital, where a home warp lands: a copy of the three
-- destinations of xi.teleport.toHomeNation (scripts/globals/teleports.lua),
-- which reads the traveller's own nation and so cannot send a cardian to
-- the player's. Any nation but Bastok and San d'Oria lands in Windurst, as
-- there. Compared with upstream's at every merge from base (CLAUDE.md).
local homeNation =
{
    [xi.nation.BASTOK]   = { 89, 0, -66, 0, 234 },
    [xi.nation.SANDORIA] = { 49, -1, 29, 164, 231 },
    [xi.nation.WINDURST] = { 193, -12, 220, 64, 240 },
}

local function capitalOf(player)
    return homeNation[player:getNation()] or homeNation[xi.nation.WINDURST]
end

-- The ids of the cardians beside him: in his party and his zone, standing
local function cardiansWith(player)
    local ids   = {}
    local party = player:getParty()
    if party == nil then
        return ids
    end

    local zone = player:getZoneID()
    for _, member in pairs(party) do
        if
            member:getID() ~= player:getID() and
            member:isCardian() and
            member:getZoneID() == zone and
            not member:isDead()
        then
            ids[#ids + 1] = member:getID()
        end
    end

    return ids
end

-- With cardians beside him and his addon there to ask, the purchase waits
-- for his answer: true when it was put to him, and nothing is bought now
local function askFirst(player, purchase)
    if not player:isPC() or player:isCardian() then
        return false
    end

    local cardians = cardiansWith(player)
    if #cardians == 0 then
        return false
    end

    return player:cardianOfferPartyWarp(purchase, cardians, patience)
end

m:addOverride('xi.conquest.teleporterOnEventFinish', function(player, csid, option, teleporterEvent)
    upstream[teleporter] = super
    if
        buysWarp(teleporter, option, csid, teleporterEvent) and
        askFirst(player, { teleporter, csid, option, teleporterEvent })
    then
        return
    end

    super(player, csid, option, teleporterEvent)
end)

m:addOverride('xi.conquest.vendorOnEventFinish', function(player, option, vendorRegion)
    upstream[vendor] = super
    if
        buysWarp(vendor, option) and
        askFirst(player, { vendor, 0, option, vendorRegion })
    then
        return
    end

    super(player, option, vendorRegion)
end)

-- The warp a handler has just sold him: a teleport effect he did not carry
-- before it ran, to an outpost or to his nation
local function warpBought(player, carriedBefore)
    if carriedBefore then
        return nil
    end

    local effect = player:getStatusEffect(xi.effect.TELEPORT)
    if effect == nil then
        return nil
    end

    local power = effect:getPower()
    if power ~= xi.teleport.id.OUTPOST and power ~= xi.teleport.id.HOME_NATION then
        return nil
    end

    return effect
end

-- His answer (the server calls it: offers.h's resolver for the party's
-- warp), with the purchase as the override held it -- { handler, csid,
-- option, teleporter event or vendor region } -- and the cardians beside him
-- when he was asked. A no buys nothing and moves nobody. A yes buys the warp
-- through upstream's own handler, which checks and charges as ever (short of
-- gil, he buys nothing); bought, it goes off on him as ever, and each cardian
-- of the question still beside him -- in his party and his zone, standing --
-- is sent to where it takes him: the outpost's own spot, or his nation's
-- capital. Carried off ahead of him, she holds where she lands until he
-- arrives (the automatic hold), and follows him from there. How many went
-- with him.
xi.cardian = xi.cardian or {}
xi.cardian.partyWarp = xi.cardian.partyWarp or {}

xi.cardian.partyWarp.resolve = function(player, yes, purchase, cardianIds)
    if not yes or purchase == nil then
        return 0
    end

    local buy = upstream[purchase[1]]
    if buy == nil then
        print(string.format('[cardian] party warp: no handler %s to buy %s\'s warp with', tostring(purchase[1]), player:getName()))
        return 0
    end

    local carriedBefore = player:hasStatusEffect(xi.effect.TELEPORT)
    if purchase[1] == teleporter then
        buy(player, purchase[2], purchase[3], purchase[4])
    else
        buy(player, purchase[3], purchase[4])
    end

    local effect = warpBought(player, carriedBefore)
    if effect == nil then
        player:printToPlayer('The warp did not go through.', xi.msg.channel.SYSTEM_3)
        return 0
    end

    local beside = {}
    for _, id in ipairs(cardiansWith(player)) do
        beside[id] = true
    end

    local went = 0
    for _, id in ipairs(cardianIds or {}) do
        local cardian = beside[id] and GetPlayerByID(id) or nil
        if cardian ~= nil then
            if effect:getPower() == xi.teleport.id.OUTPOST then
                xi.teleport.toOutpost(cardian, effect:getSubPower())
            else
                cardian:setPos(unpack(capitalOf(player)))
            end

            went = went + 1
        end
    end

    return went
end

return m
