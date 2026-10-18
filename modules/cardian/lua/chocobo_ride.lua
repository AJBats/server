-----------------------------------
-- Cardian: a cardian rents a chocobo (ROADMAP T, "Chocobos on the trek",
-- #527 and #528)
-----------------------------------
-- When the player rents a chocobo at a city's stable, every cardian of his
-- party beside him who holds the Chocobo License rents one too and comes
-- out where his rental set him down (player:cardianRidersBeside, asked
-- before his rental moves him: in his zone and within reach of him, alive,
-- on foot, in no fight, free of her errands in town, not holding on his
-- order nor in his maneuver). A rental of the past (allied notes) or a
-- chocobo race takes nobody along. On her trek a cardian rents at a city's
-- stable by herself (xi.cardian.chocobo.rent, which pawn/riding.cpp calls
-- as she stands at the stable); her trek's crossing then takes her to the
-- stable's set-down, as a zone line's would.
--
-- A body of the world rides free, the world simulating no gil for her, and
-- counts no sale; his alts and owned cardians pay the stable's own price,
-- from their own gil first and then from his while he is online (the user,
-- 2026-10-18), and one the two purses cannot pay for rents nothing.
--
-- The rental is a copy of the last steps of xi.chocobo.renterOnEventFinish
-- (scripts/globals/chocobo.lua): its time by the rider's own level, the
-- sale counted, the mount, the pet sent away and the set-down at the
-- stable's `pos`. Compare them at every merge from base.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_chocobo_ride')

xi.cardian = xi.cardian or {}
xi.cardian.chocobo = xi.cardian.chocobo or {}

-- The stable's price paid for a rider: nothing when she rides free; else
-- from her gil first and then the payer's, who may be nil. False when the
-- purses together fall short
local function charge(payer, cardian, price, free)
    if free then
        return true
    end

    local own = math.min(cardian:getGil(), price)
    local his = payer ~= nil and payer:getGil() or 0
    if own + his < price then
        return false
    end

    if own > 0 then
        cardian:delGil(own)
    end

    if price > own then
        payer:delGil(price - own)
    end

    return true
end

-- Her chocobo, as the stable's own rental gives one: a paid rental counts
-- a sale; `setDown` sets her down at the stable's `pos`
local function mount(cardian, zoneId, info, free, setDown)
    local duration = 900
    if cardian:getMainLvl() >= 20 then
        duration = 1800 + (cardian:getMod(xi.mod.CHOCOBO_RIDING_TIME) * 60)
    end

    if not free then
        xi.chocobo.increaseSales(zoneId)
    end

    cardian:addStatusEffect(xi.effect.MOUNTED, { duration = duration, origin = cardian, silent = true })
    cardian:despawnPet()
    if setDown and info.pos then
        cardian:setPos(unpack(info.pos))
    end
end

-- Her own rental at the city's stable she stands by, on her trek: `player`
-- her player while he is online (nil else), `free` whether she rides free.
-- Her trek's crossing carries her to the set-down. { rented = true }, or
-- { refusal = why }
xi.cardian.chocobo.rent = function(cardian, player, free)
    local zoneId = cardian:getZoneID()
    local info   = xi.chocobo.chocoboInfo[zoneId]
    if info == nil or info.past or info.pos == nil then
        return { refusal = 'no stable here' }
    end

    if cardian:hasStatusEffect(xi.effect.MOUNTED) then
        return { refusal = 'already riding' }
    end

    if not charge(player, cardian, xi.chocobo.getPrice(cardian), free) then
        return { refusal = 'the purses cannot pay' }
    end

    mount(cardian, zoneId, info, free, false)
    return { rented = true }
end

m:addOverride('xi.chocobo.renterOnEventFinish', function(player, csid, option, eventSucceed)
    local zoneId = player:getZoneID()
    local info   = xi.chocobo.chocoboInfo[zoneId]
    local riders = {}

    if
        info ~= nil and
        not info.past and
        csid == eventSucceed and
        option == 0 and
        player:getCharVar('[ChocoGame]DestCity') == 0 and
        not player:hasStatusEffect(xi.effect.MOUNTED)
    then
        riders = player:cardianRidersBeside()
    end

    super(player, csid, option, eventSucceed)

    if #riders == 0 or not player:hasStatusEffect(xi.effect.MOUNTED) then
        return
    end

    for _, rider in ipairs(riders) do
        local cardian = GetPlayerByID(rider.id)
        if
            cardian ~= nil and
            cardian:getZoneID() == zoneId and
            charge(player, cardian, xi.chocobo.getPrice(cardian), rider.free)
        then
            mount(cardian, zoneId, info, rider.free, true)
        end
    end
end)

return m
