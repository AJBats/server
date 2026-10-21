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
#include "common/earth_time.h"
#include "map/entities/char_entity.h"
#include "map/item_container.h"
#include "map/items/item.h"
#include "map/lua/lua_base_entity.h"
#include "map/pause/input_gate.h"
#include "map/pause/pause.h"
#include "map/pawn/ah_sell.h"
#include "map/pawn/auction.h"
#include "map/pawn/cardian_link_protocol.h"
#include "map/pawn/dbox.h"
#include "map/pawn/item_moves.h"
#include "map/pawn/players.h"
#include "map/pawn/errands.h"
#include "map/pawn/ls_bank_moves.h"
#include "map/pawn/pawn.h"
#include "map/pawn/pawn_loot.h"
#include "map/pawn/world.h"
#include "map/utils/itemutils.h"
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
        // The test triggers that refuse a row (ah_sell.lua, ah_buy.lua) are
        // switched off by their own files; a run that died with one on would
        // leave it in the test database for every suite of the next run
        db::preparedStmt("DROP TRIGGER IF EXISTS cardian_test_refuse_listing");
        db::preparedStmt("DROP TRIGGER IF EXISTS cardian_test_refuse_give");

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

        // Nobody rents with him at the stable (modules/cardian/lua/chocobo_ride.lua);
        // a test mocks the riders, each { id, free }
        ::lua["CBaseEntity"]["cardianRidersBeside"] = [](CLuaBaseEntity* /* PLuaBaseEntity */) -> sol::table
        {
            return ::lua.create_table();
        };

        // The chocobo porter's question after his rental (modules/cardian/lua/
        // chocobo_ride.lua): the porter is the pawn module's, not linked here; a
        // test stubs it to see it put
        ::lua["CBaseEntity"]["cardianPorterAsk"] = [](CLuaBaseEntity* /* PLuaBaseEntity */)
        {
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

        // Selling on the auction house as the Sell page's handlers do it
        // (scripts/tests/cardian/ah_sell.lua, pawn/ah_sell.h), on players of
        // the test's, the places they stand left out: a listing put up by a
        // seller from a holder's bag, its cancel, his sales status, and a
        // listing bought as the crowd buys one, which pays his delivery box
        ::lua["xi"]["cardian"]["ahsell"] = ::lua.create_table();
        sol::table ah                    = ::lua["xi"]["cardian"]["ahsell"];
        const auto charOf                = [](CLuaBaseEntity* PLua)
        {
            return dynamic_cast<CCharEntity*>(PLua->GetBaseEntity());
        };
        ah["status"] = ::lua.create_table_with(
            "OK", CL_S_OK, "NO_ITEM", CL_S_NO_ITEM, "ITEM_EQUIPPED", CL_S_ITEM_EQUIPPED, "NO_SPACE", CL_S_NO_SPACE,
            "NOT_ENOUGH_GIL", CL_S_NOT_ENOUGH_GIL, "NOT_AUCTIONABLE", CL_S_NOT_AUCTIONABLE, "LISTING_LIMIT", CL_S_LISTING_LIMIT,
            "NOT_A_FULL_STACK", CL_S_NOT_A_FULL_STACK, "LISTING_GONE", CL_S_LISTING_GONE, "NOT_IN_STACKS", CL_S_NOT_IN_STACKS,
            "NO_SUCH_BAG", CL_S_NO_SUCH_BAG, "ITEM_SLIPPED_AWAY", CL_S_ITEM_SLIPPED_AWAY, "BAD_PRICE", CL_S_BAD_PRICE);
        // the moves' marks table, as the pawn module's init makes it
        ah["prepare"] = []()
        {
            pawn::itemmoves::ensureTable();
        };
        // the next move failed on purpose, by item_moves.h's Fault number;
        // pending, the fault not yet met
        ah["failNext"] = [](const uint8 fault)
        {
            pawn::itemmoves::failNext(static_cast<pawn::itemmoves::Fault>(fault));
        };
        ah["pending"] = []() -> uint8
        {
            return static_cast<uint8>(pawn::itemmoves::pendingFault());
        };
        ah["fee"] = [](const uint32 price, const bool stack) -> uint32
        {
            return pawn::ahsell::feeOf(price, stack);
        };
        ah["list"] = [charOf](CLuaBaseEntity* PSeller, CLuaBaseEntity* PHolder, const uint8 loc, const uint8 slot, const uint16 itemId, const bool stack,
                              const uint32 price) -> std::tuple<uint16, uint32, uint32>
        {
            const auto listed = pawn::ahsell::list(charOf(PSeller), charOf(PHolder), loc, slot, itemId, stack, price);
            return { listed.status, listed.fee, listed.listing };
        };
        ah["cancel"] = [charOf](CLuaBaseEntity* PSeller, const uint32 listing, const uint16 itemId) -> std::tuple<uint16, uint8>
        {
            const auto cancelled = pawn::ahsell::cancel(charOf(PSeller), listing, itemId);
            return { cancelled.status, cancelled.landed };
        };
        ah["reprice"] = [charOf](CLuaBaseEntity* PSeller, const uint32 listing, const uint16 itemId, const uint32 price) -> std::tuple<uint16, uint32>
        {
            const auto repriced = pawn::ahsell::reprice(charOf(PSeller), listing, itemId, price);
            return { repriced.status, repriced.fee };
        };
        // { open = n, rows = { { listing, price, date, item, stack, sold } ... } }
        ah["listings"] = [](const uint32 charid) -> sol::table
        {
            std::size_t open = 0;
            sol::table  rows = ::lua.create_table();
            for (const auto& row : pawn::ahsell::listingsOf(charid, 48, open))
            {
                rows.add(::lua.create_table_with("listing", row.listing, "price", row.price, "date", row.date, "item", row.item, "stack", row.stack, "sold", row.sold));
            }
            return ::lua.create_table_with("open", open, "rows", rows);
        };
        // A listing's date moved back by seconds, as if it had gone up then
        ah["backdate"] = [](const uint32 listing, const uint32 seconds)
        {
            db::preparedStmt("UPDATE auction_house SET date = date - ? WHERE id = ?", seconds, listing);
        };
        // The seller a listing names, its id and its name
        ah["sellerOf"] = [](const uint32 listing) -> std::tuple<uint32, std::string>
        {
            const auto rset = db::preparedStmt("SELECT seller, seller_name FROM auction_house WHERE id = ?", listing);
            if (rset && rset->next())
            {
                return { rset->get<uint32>("seller"), rset->get<std::string>("seller_name") };
            }
            return { 0, std::string{} };
        };
        // A listing bought at price, as the crowd's buyer marks one sold: the
        // auction house's trigger pays its gil into the seller's delivery box
        ah["buy"] = [](const uint32 listing, const uint32 price)
        {
            db::preparedStmt("UPDATE auction_house SET buyer = 0, buyer_name = 'Crowd', sale = ?, sell_date = ? WHERE id = ?", price, earth_time::timestamp(), listing);
        };
        // Every sale's gil taken out of his delivery box, as collecting it does
        ah["collect"] = [](const uint32 charid)
        {
            db::preparedStmt("DELETE FROM delivery_box WHERE charid = ? AND box = 1 AND itemid = 65535 AND sender = 'AH-Jeuno'", charid);
        };
        // Every listing written to the auction house refused by the database
        // while on -- a trigger of the test database's, as a statement the
        // server refuses after the piece has left her bag. True once it stands
        // as asked
        ah["refuseListings"] = [](const bool on) -> bool
        {
            db::preparedStmt("DROP TRIGGER IF EXISTS cardian_test_refuse_listing");
            if (!on)
            {
                return true;
            }
            return db::preparedStmt("CREATE TRIGGER cardian_test_refuse_listing BEFORE INSERT ON auction_house FOR EACH ROW "
                                    "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'refused by the test'") != nullptr;
        };

        // A cardian's bid with the player's purse behind hers, as the Auction
        // House screen's bid makes it once its gates have passed
        // (scripts/tests/cardian/ah_buy.lua, pawn::auction::purchaseShared),
        // on players of the test's: the cheapest listing at or under the bid
        // bought, `fromPurse` of the price from the purse's gil. Its faults
        // are item_moves.h's, armed through xi.cardian.ahsell
        ::lua["xi"]["cardian"]["ahbuy"] = ::lua.create_table();
        sol::table buy                  = ::lua["xi"]["cardian"]["ahbuy"];
        buy["purchase"]                 = [charOf](CLuaBaseEntity* PBuyer, CLuaBaseEntity* PPurse, const uint16 itemId, const bool stack, const uint32 price, const uint32 fromPurse) -> bool
        {
            const CItem* PKind    = xi::items::lookup(itemId);
            const uint32 quantity = stack && PKind != nullptr ? PKind->getStackSize() : 1;
            return pawn::auction::purchaseShared(charOf(PBuyer), charOf(PPurse), itemId, stack, quantity, price, fromPurse);
        };
        // Whom a listing was sold to, by id and name, and what it sold for:
        // 0, '' and 0 while it is on sale
        buy["soldTo"] = [](const uint32 listing) -> std::tuple<uint32, std::string, uint32>
        {
            const auto rset = db::preparedStmt("SELECT COALESCE(buyer, 0) AS buyer, COALESCE(buyer_name, '') AS buyer_name, sale FROM auction_house WHERE id = ?", listing);
            if (rset && rset->next())
            {
                return { rset->get<uint32>("buyer"), rset->get<std::string>("buyer_name"), rset->get<uint32>("sale") };
            }
            return { 0, std::string{}, 0 };
        };
        // Every item given into a character's bags refused by the database
        // while on -- a trigger of the test database's, as a statement the
        // server refuses after the purses have paid (a purchase writes no
        // other row of char_inventory: its payments are updates). True once
        // it stands as asked
        buy["refuseGives"] = [](const bool on) -> bool
        {
            db::preparedStmt("DROP TRIGGER IF EXISTS cardian_test_refuse_give");
            if (!on)
            {
                return true;
            }
            return db::preparedStmt("CREATE TRIGGER cardian_test_refuse_give BEFORE INSERT ON char_inventory FOR EACH ROW "
                                    "SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'refused by the test'") != nullptr;
        };

        // A cardian's delivery box as the Link's DBOX drives it
        // (scripts/tests/cardian/dbox.lua, pawn/dbox.h), on players of the
        // test's, the place they stand left out: one request on a box, its
        // eight slots, and what waits beyond them
        ::lua["xi"]["cardian"]["dbox"] = ::lua.create_table();
        sol::table dbox                = ::lua["xi"]["cardian"]["dbox"];
        dbox["status"]                 = ::lua.create_table_with(
            "OK", CL_S_OK, "NO_ITEM", CL_S_NO_ITEM, "NO_SPACE", CL_S_NO_SPACE, "RARE_OWNED", CL_S_RARE_OWNED, "ITEM_EQUIPPED", CL_S_ITEM_EQUIPPED,
            "BAD_QUANTITY", CL_S_BAD_QUANTITY, "REFUSED", CL_S_REFUSED, "DBOX_BUSY", CL_S_DBOX_BUSY, "NOT_DELIVERABLE", CL_S_NOT_DELIVERABLE,
            "BOX_BACKLOGGED", CL_S_BOX_BACKLOGGED, "NO_RECIPIENT", CL_S_NO_RECIPIENT, "NO_SENDER", CL_S_NO_SENDER, "ALREADY_SENT", CL_S_ALREADY_SENT,
            "MALFORMED", CL_S_MALFORMED);
        dbox["action"] = ::lua.create_table_with(
            "LOOK", CL_DBOX_LOOK, "TAKE", CL_DBOX_TAKE, "DROP", CL_DBOX_DROP, "RETURN", CL_DBOX_RETURN,
            "SET", CL_DBOX_SET, "SEND", CL_DBOX_SEND, "RECALL", CL_DBOX_RECALL, "CLEAR", CL_DBOX_CLEAR);
        dbox["state"] = ::lua.create_table_with(
            "EMPTY", CL_DBOX_EMPTY, "WAITING", CL_DBOX_WAITING, "SENT", CL_DBOX_SENT, "DELIVERED", CL_DBOX_DELIVERED);
        // one request; nil when the test's player has no session to lend its scheduler
        dbox["act"] = [charOf](CLuaBaseEntity* PLua, const uint8 box, const uint8 action, const uint8 slot, const uint8 invSlot, const uint32 qty,
                               const uint32 to) -> sol::optional<uint16>
        {
            auto* PChar     = charOf(PLua);
            auto* scheduler = PChar != nullptr ? pawn::players::schedulerOf(PChar->id) : nullptr;
            if (scheduler == nullptr)
            {
                return sol::nullopt;
            }
            return pawn::dbox::act(PChar, box, action, slot, invSlot, qty, to, *scheduler);
        };
        // { [0..7] = { item, state, qty, name } } of a character's box
        dbox["slots"] = [](const uint32 charid, const uint8 box) -> sol::table
        {
            sol::table out = ::lua.create_table();
            const auto all = pawn::dbox::slotsOf(charid, box);
            for (std::size_t i = 0; i < all.size(); ++i)
            {
                out[i] = ::lua.create_table_with("item", all[i].item, "state", all[i].state, "qty", all[i].qty, "name", all[i].name);
            }
            return out;
        };
        dbox["waiting"] = [](const uint32 charid) -> uint8
        {
            return pawn::dbox::waitingFor(charid);
        };
        // why a set of the stack in a bag's slot would be refused, asked before
        // anything moves
        dbox["setRefusal"] = [charOf](CLuaBaseEntity* PLua, const uint8 slot, const uint8 loc, const uint8 invSlot, const uint32 qty, const uint32 to) -> uint16
        {
            auto* PChar = charOf(PLua);
            auto* PBag  = PChar != nullptr ? PChar->getStorage(loc) : nullptr;
            return pawn::dbox::setRefusal(PChar, slot, PBag != nullptr ? PBag->GetItem(invSlot) : nullptr, qty, to);
        };
    }
};

REGISTER_CPP_MODULE(CardianTestStubs);
