-----------------------------------
-- func: pawnrecruit <census name>
-- desc: Cardian recruitment (ROADMAP H) - the developer's hand on the tier a
--       cardian belongs to. A wild cardian becomes yours: her cardian_pawns
--       row changes owner from the world's account to yours and the census
--       stops offering her a slot. Standing, she changes hands on the spot
--       and stands under your name where she was; not standing, she signs in
--       with your club at your next login.
--
--       GM only, and a cheat: the real recruit verb will want an active
--       linkshell and a pearl out of your sack, at the game's own price.
--       Party membership is a separate axis -- invite her as usual after.
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 's',
}

commandObj.onTrigger = function(player, name)
    if name == nil then
        player:printToPlayer('Usage: !pawnrecruit <census name>')
        return
    end

    local err = player:cardianRecruit(name)
    if err ~= '' then
        player:printToPlayer(string.format('Cannot recruit %s: %s.', name, err))
    else
        player:printToPlayer(string.format('%s is yours now. Invite her to your party as usual.', name))
    end
end

return commandObj
