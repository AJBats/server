/*
===========================================================================

  Copyright (c) 2026 Cardian

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see http://www.gnu.org/licenses/

===========================================================================
*/

// The pawn module's side of the marked calls upstream files make into it,
// and of the Lua bindings Cardian's Lua modules call. xi_test links the
// map's libraries but not the module, whose sources are APP_SOURCES and go
// into xi_map alone, so here each gets what a server with no cardians
// does: nobody is a world body, every exp grant lands whole, nobody signs
// in or out with the player, nobody leaving a party has a trek to end,
// and nobody is a cardian.

#include "map/lua/lua_base_entity.h"
#include "map/pawn/pawn.h"
#include "map/pawn/world.h"
#include "map/utils/moduleutils.h"

namespace pawn
{
    auto signOutClub(const CCharEntity* /* PPlayer */) -> uint32
    {
        return 0;
    }

    void leftParty(const CBattleEntity* /* PMember */, const CParty* /* PParty */)
    {
    }
} // namespace pawn

namespace pawn::world
{
    auto isBody(const uint32 /* charid */) -> bool
    {
        return false;
    }

    auto capExp(const CCharEntity* /* PChar */, const uint32 exp) -> uint32
    {
        return exp;
    }
} // namespace pawn::world

// The bindings modules/cardian/lua calls: isCardian on the players it sees
// (a test that wants a cardian mocks one --
// stub('CBaseEntity.isCardian', function (entity) return ... end)), and
// cardianSignIn at login, the names of those who stood.
class CardianTestStubs : public CPPModule
{
    void OnInit() override
    {
        lua["CBaseEntity"]["isCardian"] = [](CLuaBaseEntity* /* PLuaBaseEntity */) -> bool
        {
            return false;
        };

        lua["CBaseEntity"]["cardianSignIn"] = [](CLuaBaseEntity* /* PLuaBaseEntity */) -> std::string
        {
            return "";
        };
    }
};

REGISTER_CPP_MODULE(CardianTestStubs);
