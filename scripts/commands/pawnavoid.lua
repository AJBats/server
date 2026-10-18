-----------------------------------
-- func: pawnavoid <charname> <on|off>
-- desc: Cardian pawns - aggro avoidance. On, the cardian keeps itself,
--       its formation slot and its paths outside the detection range of
--       every nearby mob that would go for her, and is pushed away as mobs
--       roam toward it. Off lets her walk past them.
--       This checks or unchecks the cardian's Avoid aggro gambit row,
--       adding one when she has none. The row is read only while she is
--       in no real player's party: in his party the Shortcuts menu's
--       Avoid aggro decides for every member. Keeping clear of the kin of her own
--       fight is a row of its own, Avoid links, set in the gambit editor.
--       (Refusing pulls with aggressive company is the Orders page's.)
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 0,
    parameters = 's',
}

commandObj.onTrigger = function(player, line)
    local name, mode = string.match(line or '', '^(%S+)%s+(%S+)')

    if name == nil or (mode ~= 'on' and mode ~= 'off') then
        player:printToPlayer('Usage: !pawnavoid <charname> <on|off>')
        return
    end

    local err = player:cardianAvoid(name, mode == 'on')
    if err ~= '' then
        player:printToPlayer(string.format('Cannot set aggro avoidance: %s.', err))
    else
        player:printToPlayer(string.format('%s aggro avoidance %s. In your party, the Shortcuts switch decides.', name, mode))
    end
end

return commandObj
