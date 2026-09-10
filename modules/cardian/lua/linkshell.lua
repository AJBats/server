-----------------------------------
-- Cardian: the club signs in with you (ROADMAP H)
--
-- At login every character of the player's account and every cardian the
-- account owns stands where the game last saved her -- her own zone and
-- position, never brought to the player -- waiting to be invited. They
-- sign out with the player (charutils::removeCharFromZone). The player
-- hears who stood and where, one line in the chat log.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('cardian_linkshell')

m:addOverride('xi.player.onGameIn', function(player, firstLogin, zoning)
    super(player, firstLogin, zoning)
    if zoning then
        return
    end
    local stood = player:cardianSignIn()
    if stood ~= '' then
        player:printToPlayer(string.format('Signed in with you: %s.', stood), xi.msg.channel.SYSTEM_3)
    end
end)

return m
