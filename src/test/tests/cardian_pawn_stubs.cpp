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
// nobody's followers set out ahead of him, nobody lands beside him in a
// battlefield, no warp of his is held for a question, and nobody is a cardian.

#include "common/database.h"
#include "map/entities/char_entity.h"
#include "map/item_container.h"
#include "map/items/item.h"
#include "map/lua/lua_base_entity.h"
#include "map/pause/input_gate.h"
#include "map/pause/pause.h"
#include "map/pawn/errands.h"
#include "map/pawn/ls_bank_moves.h"
#include "map/pawn/pawn.h"
#include "map/pawn/pawn_loot.h"
#include "map/pawn/world.h"
#include "map/utils/moduleutils.h"

#include <tuple>
#include <unordered_set>

namespace pawn
{
    auto signOutClub(CCharEntity* /* PPlayer */) -> uint32
    {
        return 0;
    }

    void leftParty(const CBattleEntity* /* PMember */, const CParty* /* PParty */)
    {
    }

    void playerZoning(const CCharEntity* /* PPlayer */, const xi::ZoneId /* destination */)
    {
    }

    void landWithPlayer(const CCharEntity* /* PPlayer */, const position_t& /* landing */)
    {
    }
} // namespace pawn

namespace pawn::errands
{
    void endForLogin(const uint32 /* charid */)
    {
    }
} // namespace pawn::errands

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

namespace pawn::loot
{
    auto voids(const CCharEntity* /* PWinner */) -> bool
    {
        return false;
    }
} // namespace pawn::loot

// The bindings modules/cardian/lua calls: isCardian on the players it sees
// (a test that wants a cardian mocks one --
// stub('CBaseEntity.isCardian', function (entity) return ... end)), and
// cardianSignIn at login, the names of those who stood.
class CardianTestStubs : public CPPModule
{
    void OnInit() override
    {
        ::lua["CBaseEntity"]["isCardian"] = [](CLuaBaseEntity* /* PLuaBaseEntity */) -> bool
        {
            return false;
        };

        ::lua["CBaseEntity"]["cardianSignIn"] = [](CLuaBaseEntity* /* PLuaBaseEntity */) -> std::string
        {
            return "";
        };

        // A mocked cardian is the player's own: the mirror applies to her
        ::lua["CBaseEntity"]["cardianOwns"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const std::string& /* name */) -> bool
        {
            return true;
        };

        // The party memory's affinity, grown by the mirror at a completion,
        // and her contract: a mocked cardian is the player's own, under none
        ::lua["CBaseEntity"]["cardianBond"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const std::string& /* name */, const std::string& /* why */, sol::optional<bool> /* mission */)
        {
        };
        ::lua["CBaseEntity"]["cardianContract"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const uint32 /* playerCharID */) -> std::string
        {
            return "";
        };

        // A grant told to the account (modules/cardian/lua/account_wide.lua):
        // the account-wide copying is the pawn module's, not linked here
        ::lua["CBaseEntity"]["cardianAccountGranted"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const std::string& /* what */)
        {
        };

        // The party's warp held for his answer: no addon is bound to ask him,
        // so the warp is bought as upstream buys it (a test that wants the
        // question put stubs this to true)
        ::lua["CBaseEntity"]["cardianOfferPartyWarp"] = [](CLuaBaseEntity* /* PLuaBaseEntity */, const sol::table& /* purchase */, const sol::table& /* cardians */, const uint32 /* seconds */) -> bool
        {
            return false;
        };

        // The combat pause is core, so this is the real manager, not a stub: what a
        // Lua test needs to hold the simulation and let it go (scripts/tests/cardian/
        // pause.lua). Held by nobody in particular, so no holder can go offline.
        ::lua["xi"]["cardian"].get_or_create<sol::table>();
        ::lua["xi"]["cardian"]["pause"]            = ::lua.create_table();
        ::lua["xi"]["cardian"]["pause"]["hold"]    = []() -> bool
        {
            return cardian::pause::hold(0, "a test") == cardian::pause::Result::Ok;
        };
        ::lua["xi"]["cardian"]["pause"]["release"] = []() -> bool
        {
            return cardian::pause::release("a test") == cardian::pause::Result::Ok;
        };
        // A test that let real time go by in a hold leaves the calendar as it found it
        ::lua["xi"]["cardian"]["pause"]["forgetDrift"] = []()
        {
            earth_time::calendar_state.store(0);
        };
        // Real seconds going by on the Earth clock, which the harness's skipTime leaves
        // alone: it moves the simulation clock only
        ::lua["xi"]["cardian"]["pause"]["realSecondsGoBy"] = [](const uint32 seconds)
        {
            earth_time::add_offset(std::chrono::seconds(seconds));
        };
        ::lua["xi"]["cardian"]["pause"]["isHeld"]  = []() -> bool
        {
            return cardian::pause::isHeld();
        };
        // The command a character has waiting for the release, as its packet id and,
        // for 0x01A, its action id; nothing when he has none.
        ::lua["xi"]["cardian"]["pause"]["queued"] = [](const uint32 charid) -> std::tuple<sol::optional<uint16>, sol::optional<uint16>>
        {
            if (const auto command = cardian::pause::input::queued(charid))
            {
                return { command->packetId, command->actionId };
            }
            return { sol::nullopt, sol::nullopt };
        };

        // The shell's bank's moves as xi_test's commit cases drive them
        // (scripts/tests/cardian/ls_bank_commit.lua, #478): a move made on a
        // player of the test's as the holder of a shell of the test's, the
        // next move failed on purpose (failNext, by moves::MoveFault's
        // number; pending, the fault not yet met), and the bank's rows and
        // the player's item rows read back, to set beside him in memory
        namespace moves                  = pawn::lsbank::moves;
        ::lua["xi"]["cardian"]["lsbank"] = ::lua.create_table();
        sol::table bank                  = ::lua["xi"]["cardian"]["lsbank"];
        const auto holderOf              = [](CLuaBaseEntity* PLua, const uint32 lsid)
        {
            pawn::lsbank::Shell shell;
            shell.lsid = lsid;
            shell.rank = CL_RANK_HOLDER;
            return std::pair{ dynamic_cast<CCharEntity*>(PLua->GetBaseEntity()), shell };
        };
        bank["prepare"] = [](const uint32 lsid)
        {
            moves::ensureTables();
            db::preparedStmt("DELETE FROM cardian_ls_bank WHERE lsid = ?", lsid);
            db::preparedStmt("DELETE FROM cardian_ls_bank_head WHERE lsid = ?", lsid);
        };
        bank["failNext"] = [](const uint8 fault)
        {
            moves::failNext(static_cast<moves::MoveFault>(fault));
        };
        bank["pending"] = []() -> uint8
        {
            return static_cast<uint8>(moves::pendingFault());
        };
        bank["deposit"] = [holderOf](CLuaBaseEntity* PLua, const uint32 lsid, const uint8 loc, const uint8 slot, const uint32 qty) -> uint16
        {
            const auto [PChar, shell] = holderOf(PLua, lsid);
            const CItem* PItem        = PChar->getStorage(loc)->GetItem(slot);
            cl_bank_deposit ask{};
            ask.cardian  = PChar->id;
            ask.loc      = loc;
            ask.slot     = slot;
            ask.qty      = qty;
            ask.item     = PItem != nullptr ? PItem->getID() : 0;
            uint8 landed = 0;
            return moves::deposit(PChar, shell, ask, landed);
        };
        bank["withdraw"] = [holderOf](CLuaBaseEntity* PLua, const uint32 lsid, const uint8 bankSlot, const uint32 qty, const uint8 loc) -> uint16
        {
            const auto [PChar, shell] = holderOf(PLua, lsid);
            const auto stacks         = moves::readStacks(lsid).value_or(std::vector<cardian::lsbank::Stack>{});
            cl_bank_withdraw ask{};
            ask.cardian  = PChar->id;
            ask.slot     = bankSlot;
            ask.qty      = qty;
            ask.item     = bankSlot >= 1 && bankSlot <= stacks.size() ? stacks[bankSlot - 1].item : 0;
            ask.loc      = loc;
            ask.version  = static_cast<uint16>(moves::readHead(lsid).version & 0xFFFF);
            uint8 landed = 0;
            return moves::withdraw(PChar, shell, ask, landed);
        };
        bank["gil"] = [holderOf](CLuaBaseEntity* PLua, const uint32 lsid, const uint32 amount, const bool out) -> uint16
        {
            const auto [PChar, shell] = holderOf(PLua, lsid);
            cl_bank_gil ask{};
            ask.cardian    = PChar->id;
            ask.amount     = amount;
            ask.out        = out ? 1 : 0;
            uint32 bankGil = 0;
            return moves::gil(PChar, shell, ask, bankGil);
        };
        // { { item, qty } ... } in the bank's order
        bank["stacks"] = [](const uint32 lsid) -> sol::table
        {
            sol::table out = ::lua.create_table();
            for (const auto& stack : moves::readStacks(lsid).value_or(std::vector<cardian::lsbank::Stack>{}))
            {
                out.add(::lua.create_table_with("item", stack.item, "qty", stack.qty));
            }
            return out;
        };
        bank["gilOf"] = [](const uint32 lsid) -> uint32
        {
            return moves::readHead(lsid).gil;
        };
        // Each stack of a bag the list names into the bank (moves::depositEach,
        // the list standing in for the junk rule); look moves nothing. The
        // outcome, the stacks moved, whether the bank filled, and the stacks
        // moved as { { slot, item, qty } ... }
        bank["depositEach"] = [holderOf](CLuaBaseEntity* PLua, const uint32 lsid, const uint8 loc, const sol::table& ids, const bool look) -> std::tuple<uint16, uint8, uint8, sol::table>
        {
            const auto [PChar, shell] = holderOf(PLua, lsid);
            std::unordered_set<uint16> named;
            for (const auto& [_, id] : ids)
            {
                named.insert(id.as<uint16>());
            }
            cl_bank_junk answer{};
            const auto   status = moves::depositEach(PChar, shell, loc, [&](const uint16 itemId) { return named.contains(itemId); }, look, answer);
            sol::table   moved  = ::lua.create_table();
            for (uint8 i = 0; i < answer.count; ++i)
            {
                moved.add(::lua.create_table_with("slot", answer.items[i].slot, "item", answer.items[i].id, "qty", answer.items[i].qty));
            }
            return { status, answer.count, answer.full, moved };
        };
        // n stacks of items of the test's own in the bank, its rows written straight
        bank["fill"] = [](const uint32 lsid, const uint8 n)
        {
            for (uint8 slot = 1; slot <= n; ++slot)
            {
                db::preparedStmt("INSERT INTO cardian_ls_bank (lsid, slot, itemId, quantity) VALUES (?, ?, ?, 1)", lsid, slot, 20000 + slot);
            }
        };
        // { [slot] = { item, qty } } of a character's rows in a bag, slot 0 (gil) included
        bank["rows"] = [](const uint32 charid, const uint8 loc) -> sol::table
        {
            sol::table out  = ::lua.create_table();
            const auto rset = db::preparedStmt("SELECT slot, itemId, quantity FROM char_inventory WHERE charid = ? AND location = ?", charid, loc);
            while (rset && rset->next())
            {
                out[rset->get<uint8>("slot")] = ::lua.create_table_with("item", rset->get<uint16>("itemId"), "qty", rset->get<uint32>("quantity"));
            }
            return out;
        };
    }
};

REGISTER_CPP_MODULE(CardianTestStubs);
