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
// nobody's followers set out ahead of him, and nobody is a cardian.

#include "map/lua/lua_base_entity.h"
#include "map/pause/input_gate.h"
#include "map/pause/pause.h"
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

    void playerZoning(const CCharEntity* /* PPlayer */, const xi::ZoneId /* destination */)
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

        // A mocked cardian is the player's own: the mirror applies to her
        lua["CBaseEntity"]["cardianOwns"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const std::string& /* name */) -> bool
        {
            return true;
        };

        // The party memory's affinity, grown by the mirror at a completion,
        // and her contract: a mocked cardian is the player's own, under none
        lua["CBaseEntity"]["cardianBond"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const std::string& /* name */, const std::string& /* why */, sol::optional<bool> /* mission */)
        {
        };
        lua["CBaseEntity"]["cardianContract"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const uint32 /* playerCharID */) -> std::string
        {
            return "";
        };

        // The combat pause is core, so this is the real manager, not a stub: what a
        // Lua test needs to hold the simulation and let it go (scripts/tests/cardian/
        // pause.lua). Held by nobody in particular, so no holder can go offline.
        lua["xi"]["cardian"].get_or_create<sol::table>();
        lua["xi"]["cardian"]["pause"]            = lua.create_table();
        lua["xi"]["cardian"]["pause"]["hold"]    = []() -> bool
        {
            return cardian::pause::hold(0, "a test") == cardian::pause::Result::Ok;
        };
        lua["xi"]["cardian"]["pause"]["release"] = []() -> bool
        {
            return cardian::pause::release("a test") == cardian::pause::Result::Ok;
        };
        // A test that let real time go by in a hold leaves the calendar as it found it
        lua["xi"]["cardian"]["pause"]["forgetDrift"] = []()
        {
            earth_time::calendar_state.store(0);
        };
        // Real seconds going by on the Earth clock, which the harness's skipTime leaves
        // alone: it moves the simulation clock only
        lua["xi"]["cardian"]["pause"]["realSecondsGoBy"] = [](const uint32 seconds)
        {
            earth_time::add_offset(std::chrono::seconds(seconds));
        };
        lua["xi"]["cardian"]["pause"]["isHeld"]  = []() -> bool
        {
            return cardian::pause::isHeld();
        };
        // The command a character has waiting for the release, as its packet id and,
        // for 0x01A, its action id; nothing when he has none.
        lua["xi"]["cardian"]["pause"]["queued"] = [](const uint32 charid) -> std::tuple<sol::optional<uint16>, sol::optional<uint16>>
        {
            if (const auto command = cardian::pause::input::queued(charid))
            {
                return { command->packetId, command->actionId };
            }
            return { sol::nullopt, sol::nullopt };
        };
    }
};

REGISTER_CPP_MODULE(CardianTestStubs);
