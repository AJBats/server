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

#include "venture_sales.h"

#include "angler.h"
#include "bodies.h"
#include "cardian_link_protocol.h"
#include "errands.h"
#include "pawn.h"
#include "pawn_items.h"
#include "players.h"
#include "seats.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/transactions/item_claim.h"
#include "lua/luautils.h"
#include "packets/c2s/0x04e_auc.h"
#include "packets/s2c/0x04c_auc.h"
#include "persist_batch.h"
#include "utils/auctionutils.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <ctime>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::sales
{
    namespace
    {
        constexpr auto   kTickEvery     = std::chrono::seconds(10);
        constexpr auto   kFadedEvery    = std::chrono::seconds(60); // a faded member's character is loaded to sell at most this often
        constexpr uint16 kGil           = 0xFFFF;                   // gil, in a delivery box
        constexpr auto   kAuctionHouse  = "AH-Jeuno";               // the sender of what the auction house leaves in a delivery box (sql/triggers.sql)
        constexpr uint8  kCarried       = CL_VENTURE_CARRIED;
        constexpr uint8  kListed        = CL_VENTURE_LISTED;
        constexpr uint8  kSold          = CL_VENTURE_SOLD;
        constexpr uint8  kReturned      = CL_VENTURE_RETURNED;
        constexpr uint8  kKept          = CL_VENTURE_KEPT;

        timer::time_point                                  tickedAt{};
        std::unordered_map<uint32, timer::time_point>      fadedAt;   // when a faded member was last loaded to sell
        std::unordered_map<uint64, timer::time_point>      weighedAt; // when a piece's weighing was last in the log, by charid and item

        auto realNow() -> uint32
        {
            return static_cast<uint32>(std::time(nullptr));
        }

        auto nameOf(const uint32 charid) -> std::string
        {
            return pawn::seats::nameOf(charid);
        }

        // ---- her character ----------------------------------------------------

        // Her character as it is in memory: her venture's body, her stand, or
        // her player's character while he plays her; none while she is faded
        auto loaded(const uint32 charid) -> CCharEntity*
        {
            if (auto* PBody = pawn::angler::bodyOf(charid); PBody != nullptr)
            {
                return PBody;
            }
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn;
            }
            return zoneutils::GetChar(charid);
        }

        auto played(const CCharEntity* PChar) -> bool
        {
            return PChar != nullptr && PChar->PSession != nullptr && !pawn::isPawn(PChar);
        }

        // What a body in no zone was told is nobody's to read
        void quiet(CCharEntity* PChar, const uint32 charid, const bool borrowed)
        {
            if (borrowed || pawn::angler::bodyOf(charid) == PChar)
            {
                PChar->clearPacketList();
            }
        }

        // Gil into her purse: in memory where she is loaded, else her saved row
        void credit(const uint32 charid, const uint32 gil)
        {
            if (auto* PChar = loaded(charid); PChar != nullptr)
            {
                auto transaction = ItemClaimTransaction::start(PChar);
                if (!transaction || !transaction->earn(gil) || !transaction->commit())
                {
                    ShowErrorFmt("sales: {} could not take {} gil into her purse", nameOf(charid), gil);
                }
                quiet(PChar, charid, false);
                return;
            }
            db::preparedStmt("UPDATE char_inventory SET quantity = quantity + ? WHERE charid = ? AND location = 0 AND slot = 0", gil, charid);
        }

        auto gilOf(CCharEntity* PChar) -> uint32
        {
            const auto* PGil = PChar->getStorage(LOC_INVENTORY)->GetItem(0);
            return PGil != nullptr && PGil->isType(ITEM_CURRENCY) ? PGil->getQuantity() : 0;
        }

        auto inInventory(CCharEntity* PChar, const uint16 item) -> uint32
        {
            auto*  storage = PChar->getStorage(LOC_INVENTORY);
            uint32 count   = 0;
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == item && PItem->getCharPrice() == 0)
                {
                    count += PItem->getQuantity();
                }
            }
            return count;
        }

        // The last inventory slot holding the item: after her bag is sorted, a
        // partial stack's, so what goes to a vendor leaves a full stack to list
        auto lastSlotOf(CCharEntity* PChar, const uint16 item) -> uint8
        {
            auto* storage = PChar->getStorage(LOC_INVENTORY);
            for (uint8 slot = storage->GetSize(); slot >= 1; --slot)
            {
                const auto* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getID() == item && PItem->getCharPrice() == 0)
                {
                    return slot;
                }
            }
            return 0;
        }

        // An inventory slot holding the item: a full stack when `full`
        auto slotOf(CCharEntity* PChar, const uint16 item, const bool full) -> uint8
        {
            auto* storage = PChar->getStorage(LOC_INVENTORY);
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                const auto* PItem = storage->GetItem(slot);
                if (PItem != nullptr && PItem->getID() == item && PItem->getCharPrice() == 0 && (!full || PItem->getQuantity() == PItem->getStackSize()))
                {
                    return slot;
                }
            }
            return 0;
        }

        // ---- the report -------------------------------------------------------

        // A part of an item's rows, by venture: from an earlier one's, and
        // from this one's
        struct Part
        {
            uint32 earlier = 0;
            uint32 current = 0;

            auto total() const -> uint32
            {
                return earlier + current;
            }
        };

        // `quantity` of an item at `where`, taken from an earlier venture's
        // rows first: the oldest catch goes first
        auto split(const uint32 charid, const uint16 item, const uint8 where, const uint32 quantity) -> Part
        {
            const auto   rset = db::preparedStmt("SELECT quantity FROM cardian_venture_items WHERE charid = ? AND earlier = 1 AND item = ? AND `where` = ?", charid,
                                                 item, where);
            const uint32 held = rset && rset->next() ? rset->get<uint32>("quantity") : 0;
            const uint32 old  = std::min(held, quantity);
            return { .earlier = old, .current = quantity - old };
        }

        // A part taken off an item's rows at `from`, each venture's value cut
        // in proportion
        void takeOff(const uint32 charid, const uint16 item, const uint8 from, const Part& part)
        {
            for (const auto& [earlier, n] : { std::pair<uint8, uint32>{ 1, part.earlier }, std::pair<uint8, uint32>{ 0, part.current } })
            {
                if (n == 0)
                {
                    continue;
                }
                db::preparedStmt("UPDATE cardian_venture_items SET value = IF(quantity > ?, value - value * ? DIV quantity, 0), quantity = IF(quantity > ?, quantity - ?, 0) "
                                 "WHERE charid = ? AND earlier = ? AND item = ? AND `where` = ?",
                                 n, n, n, n, charid, earlier, item, from);
                db::preparedStmt("DELETE FROM cardian_venture_items WHERE charid = ? AND earlier = ? AND item = ? AND `where` = ? AND quantity = 0", charid, earlier,
                                 item, from);
            }
        }

        // ... and put on at `to`, worth `value` in all, shared by count
        void putOn(const uint32 charid, const uint16 item, const uint8 to, const Part& part, const uint32 value)
        {
            const uint32 oldValue = part.total() > 0 ? static_cast<uint32>(static_cast<uint64>(value) * part.earlier / part.total()) : 0;
            for (const auto& [earlier, n, v] : { std::tuple<uint8, uint32, uint32>{ 1, part.earlier, oldValue }, std::tuple<uint8, uint32, uint32>{ 0, part.current, value - oldValue } })
            {
                if (n == 0)
                {
                    continue;
                }
                db::preparedStmt("INSERT INTO cardian_venture_items (charid, earlier, item, `where`, quantity, value) VALUES (?, ?, ?, ?, ?, ?) "
                                 "ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity), value = value + VALUES(value)",
                                 charid, earlier, item, to, n, v);
            }
        }

        // A part of an item moved between the report's rows: `value` what it
        // is worth where it lands
        void move(const uint32 charid, const uint16 item, const uint8 from, const uint8 to, const Part& part, const uint32 value)
        {
            takeOff(charid, item, from, part);
            putOn(charid, item, to, part, value);
        }

        // This venture's share of `gil` over a part: what an earlier
        // venture's catch fetches is not this one's
        auto share(const int64 gil, const Part& part) -> int32
        {
            return part.total() > 0 ? static_cast<int32>(gil * part.current / part.total()) : 0;
        }

        // What this venture's catch has fetched, less the auction house's fees
        void earned(const uint32 charid, const int32 gil)
        {
            if (gil != 0)
            {
                db::preparedStmt("UPDATE cardian_venture_report SET gil_earned = gil_earned + CAST(? AS SIGNED), updated = ? WHERE charid = ?", gil, realNow(),
                                 charid);
            }
        }

        // The report's phase once her venture is over: selling while anything
        // of the line is in her bags or listed
        void restate(const uint32 charid)
        {
            db::preparedStmt("UPDATE cardian_venture_report r SET phase = IF(r.phase = ?, r.phase, IF((SELECT COUNT(*) FROM cardian_venture_items i "
                             "WHERE i.charid = r.charid AND i.`where` IN (?, ?)) > 0, ?, ?)) WHERE r.charid = ?",
                             static_cast<uint8>(CL_VENTURE_WORKING), kCarried, kListed, static_cast<uint8>(CL_VENTURE_SELLING), static_cast<uint8>(CL_VENTURE_DONE),
                             charid);
        }

        // ---- her listings -----------------------------------------------------

        // Her listings settled: one sold, the gil the auction house left in
        // her delivery box taken into her purse; one gone unsold, sent home to
        // her delivery box, where it stays for her player
        void settle(const uint32 charid)
        {
            const auto rset = db::preparedStmt("SELECT l.id, l.item, l.quantity, l.earlier, a.id AS live, a.sale FROM cardian_venture_listings l "
                                               "LEFT JOIN auction_house a ON a.id = l.id WHERE l.charid = ?",
                                               charid);
            while (rset && rset->next())
            {
                const auto   id       = rset->get<uint32>("id");
                const auto   item     = rset->get<uint16>("item");
                const auto   quantity = rset->get<uint32>("quantity");
                const auto   old      = std::min(rset->get<uint32>("earlier"), quantity);
                const Part   part{ .earlier = old, .current = quantity - old };
                const bool   live     = !rset->isNull("live");
                const uint32 sale     = live ? rset->get<uint32>("sale") : 0;
                if (live && sale == 0)
                {
                    continue; // still up
                }
                if (sale > 0)
                {
                    const auto taken = db::preparedStmt("DELETE FROM delivery_box WHERE charid = ? AND box = 1 AND itemid = ? AND itemsubid = ? AND quantity = ? "
                                                        "AND sender = ? LIMIT 1",
                                                        charid, kGil, item, sale, std::string(kAuctionHouse));
                    if (taken && taken->rowsAffected() == 1)
                    {
                        credit(charid, sale);
                    }
                    move(charid, item, kListed, kSold, part, sale);
                    earned(charid, share(sale, part));
                    ShowInfoFmt("sales: {}'s item {} x{} sold for {} gil", nameOf(charid), item, quantity, sale);
                }
                else
                {
                    move(charid, item, kListed, kReturned, part, 0);
                    ShowInfoFmt("sales: {}'s item {} x{} came back unsold, to her delivery box", nameOf(charid), item, quantity);
                }
                db::preparedStmt("DELETE FROM cardian_venture_listings WHERE id = ?", id);
            }
        }

        // ---- the price --------------------------------------------------------

        // The server's own listing fee (auctionutils::ProofOfPurchase), for
        // judging a form before it is listed: the commit charges its own
        auto feeOf(const uint32 price, const bool stack) -> uint32
        {
            const auto fee = stack ? settings::get<uint32>("map.AH_BASE_FEE_STACKS") + price * settings::get<float>("map.AH_TAX_RATE_STACKS") / 100
                                   : settings::get<uint32>("map.AH_BASE_FEE_SINGLE") + price * settings::get<float>("map.AH_TAX_RATE_SINGLE") / 100;
            return std::clamp<uint32>(static_cast<uint32>(fee), 0, settings::get<uint32>("map.AH_MAX_FEE"));
        }

        // A form of an item on the auction house, as the crowd trades it
        struct Form
        {
            bool   ok    = false; // the crowd bought it within its listing window
            uint32 price = 0;     // what she asks: the next buyer's pick
            uint32 fee   = 0;
            float  unit  = 0.0f;  // what a unit of it nets her
        };
        auto formOf(const uint32 charid, const uint16 item, const bool stack, const uint32 units) -> Form
        {
            const uint32 window = settings::get<uint32>("pawn.VENTURE_SALE_WINDOW_DAYS") * 86400;
            const auto   rset   = db::preparedStmt("SELECT sale FROM auction_house WHERE itemid = ? AND stack = ? AND sale > 0 AND sell_date >= ? "
                                                   "ORDER BY sell_date DESC LIMIT 10",
                                                   item, static_cast<uint8>(stack ? 1 : 0), realNow() - window);
            std::vector<uint32> sales;
            while (rset && rset->next())
            {
                sales.push_back(rset->get<uint32>("sale"));
            }
            if (sales.empty())
            {
                return {};
            }
            std::ranges::sort(sales);
            uint32 price = sales[sales.size() / 2]; // the going rate: the median of its last sales
            if (const auto low = db::preparedStmt("SELECT MIN(price) AS low FROM auction_house WHERE itemid = ? AND stack = ? AND sale = 0 AND seller <> ?",
                                                  item, static_cast<uint8>(stack ? 1 : 0), charid);
                low && low->next() && !low->isNull("low"))
            {
                price = std::min(price, std::max<uint32>(low->get<uint32>("low"), 2) - 1); // a gil under the cheapest listed
            }
            const uint32 fee = feeOf(price, stack);
            return { .ok = price > fee, .price = price, .fee = fee, .unit = price > fee ? static_cast<float>(price - fee) / static_cast<float>(units) : 0.0f };
        }

        auto slotsFree(const uint32 charid) -> uint32
        {
            const uint32 limit = settings::get<uint8>("map.AH_LIST_LIMIT");
            if (limit == 0)
            {
                return UINT32_MAX;
            }
            const auto rset = db::preparedStmt("SELECT COUNT(*) AS up FROM auction_house WHERE seller = ? AND sale = 0", charid);
            const auto up   = rset && rset->next() ? rset->get<uint32>("up") : limit;
            return up < limit ? limit - up : 0;
        }

        // ---- selling ----------------------------------------------------------

        void vendor(CCharEntity* PChar, uint32 charid, uint16 item, uint32 count, uint32 unit, bool borrowed);

        // One listing up through the server's own commit; false when it did
        // not go (no slot free, nothing to list). Her purse short of the fee,
        // one of the piece goes to a vendor first to raise it
        auto list(CCharEntity* PChar, const uint32 charid, const uint16 item, const bool stack, const Form& form, const uint32 units, const uint32 vendorUnit,
                  const bool borrowed) -> bool
        {
            if (slotsFree(charid) == 0)
            {
                return false;
            }
            if (gilOf(PChar) < form.fee && vendorUnit > 0)
            {
                vendor(PChar, charid, item, 1, vendorUnit, borrowed);
            }
            const uint8 slot = slotOf(PChar, item, stack);
            if (slot == 0 || gilOf(PChar) < form.fee)
            {
                return false;
            }
            const auto before = db::preparedStmt("SELECT COALESCE(MAX(id), 0) AS top FROM auction_house WHERE seller = ?", charid);
            const auto top    = before && before->next() ? before->get<uint32>("top") : 0;
            const uint32 quantity = PChar->getStorage(LOC_INVENTORY)->GetItem(slot)->getQuantity();
            auctionutils::ProofOfPurchase(PChar, GP_AUC_PARAM_LOT{ .LimitPrice = form.price, .ItemWorkIndex = slot, .padding00 = 0, .ItemStacks = stack ? 0u : 1u });
            // the auction house's answer, as her client would read it (GP_SERV_COMMAND_AUC's Result)
            int answer = -1;
            for (const auto& packet : PChar->getPacketList())
            {
                if (packet->getType() == std::to_underlying(PacketS2C::GP_SERV_COMMAND_AUC))
                {
                    answer = packet->ref<uint8>(0x06);
                }
            }
            quiet(PChar, charid, borrowed);
            const auto after = db::preparedStmt("SELECT COALESCE(MAX(id), 0) AS top FROM auction_house WHERE seller = ? AND itemid = ? AND sale = 0", charid, item);
            const auto id    = after && after->next() ? after->get<uint32>("top") : 0;
            if (id <= top)
            {
                ShowWarningFmt("sales: {} could not list item {} (slot {}, {} held, purse {}, fee {}): the auction house answered {}", nameOf(charid), item, slot,
                               quantity, gilOf(PChar), form.fee, answer < 0 ? std::string("nothing") : std::to_string(answer));
                return false;
            }
            const auto part = split(charid, item, kCarried, units);
            db::preparedStmt("INSERT INTO cardian_venture_listings (id, charid, item, quantity, earlier, price, posted) VALUES (?, ?, ?, ?, ?, ?, ?)", id, charid,
                             item, units, part.earlier, form.price, realNow());
            move(charid, item, kCarried, kListed, part, form.price);
            earned(charid, -share(form.fee, part));
            ShowInfoFmt("sales: {} lists item {} x{} for {} gil (fee {})", nameOf(charid), item, units, form.price, form.fee);
            return true;
        }

        // Sold to a vendor at the game's own price, at once
        void vendor(CCharEntity* PChar, const uint32 charid, const uint16 item, const uint32 count, const uint32 unit, const bool borrowed)
        {
            uint32 sold = 0;
            while (sold < count)
            {
                const uint8 slot = lastSlotOf(PChar, item);
                if (slot == 0)
                {
                    break;
                }
                const uint32 n           = std::min(PChar->getStorage(LOC_INVENTORY)->GetItem(slot)->getQuantity(), count - sold);
                auto         transaction = ItemClaimTransaction::start(PChar);
                if (!transaction || !transaction->take(LOC_INVENTORY, slot, n) || !transaction->earn(n * unit) || !transaction->commit())
                {
                    ShowErrorFmt("sales: {} could not sell item {} to a vendor", nameOf(charid), item);
                    break;
                }
                sold += n;
            }
            quiet(PChar, charid, borrowed);
            if (sold > 0)
            {
                const auto part = split(charid, item, kCarried, sold);
                move(charid, item, kCarried, kSold, part, sold * unit);
                earned(charid, share(static_cast<int64>(sold) * unit, part));
                ShowInfoFmt("sales: {} sells item {} x{} to a vendor for {} gil", nameOf(charid), item, sold, sold * unit);
            }
        }

        // What nobody will take, once her venture is over, out of her line:
        // kept in her bags, so the line can end
        void keep(const uint32 charid, const uint16 item, const uint32 count)
        {
            move(charid, item, kCarried, kKept, split(charid, item, kCarried, count), 0);
            ShowInfoFmt("sales: {} keeps item {} x{}: no vendor buys it, and the auction house has no buyer", nameOf(charid), item, count);
        }

        // One piece of her line, where it pays her most. The vendor at least
        // as good as the auction house, or no buyer there: all of it to the
        // vendor at once. Else full stacks on the auction house where the crowd
        // buys them, the odd ones waiting for a full stack while a venture of
        // hers is under way and sold singly once none is; or singly, where the
        // crowd buys only singles. What neither a vendor nor the crowd takes
        // waits while she ventures, and is kept once she does not
        void sellPiece(CCharEntity* PChar, const uint32 charid, const uint16 item, const uint32 count, const bool venturing, const bool borrowed)
        {
            const auto*  PKind      = xi::items::lookup(item);
            const uint32 stackSize  = PKind != nullptr ? std::max<uint32>(PKind->getStackSize(), 1) : 1;
            const uint8  fameArea   = std::min<uint8>(PChar->profile.nation, 2);
            const auto   vendorUnit = luautils::callGlobal<uint32>("xi.shop.onSellPriceCheck", PChar, item, fameArea);
            const Form   stack      = stackSize > 1 ? formOf(charid, item, true, stackSize) : Form{};
            const Form   single     = formOf(charid, item, false, 1);
            const float  bestAh     = std::max(stack.ok ? stack.unit : 0.0f, single.ok ? single.unit : 0.0f);
            if (auto& at = weighedAt[(static_cast<uint64>(charid) << 16) | item]; timer::now() - at >= std::chrono::minutes(10))
            {
                at = timer::now();
                ShowInfoFmt("sales: {} weighs item {} x{}: a vendor pays {} each, the auction house nets {:.1f} a stack's unit ({}), {:.1f} singly ({})",
                            nameOf(charid), item, count, vendorUnit, stack.unit, stack.ok ? "the crowd buys stacks" : "no stack buyers", single.unit,
                            single.ok ? "the crowd buys singles" : "no single buyers");
            }
            // what she carries is worth what it would net her now
            db::preparedStmt("UPDATE cardian_venture_items SET value = quantity * ? WHERE charid = ? AND item = ? AND `where` = ?",
                             std::max(static_cast<uint32>(bestAh), vendorUnit), charid, item, kCarried);
            if (bestAh <= static_cast<float>(vendorUnit))
            {
                if (vendorUnit > 0)
                {
                    vendor(PChar, charid, item, count, vendorUnit, borrowed);
                }
                else if (!venturing)
                {
                    keep(charid, item, count);
                }
                return;
            }
            uint32 left = count;
            if (stack.ok && stack.unit > static_cast<float>(vendorUnit))
            {
                while (left >= stackSize && list(PChar, charid, item, true, stack, stackSize, vendorUnit, borrowed))
                {
                    left -= stackSize;
                }
                if (left >= stackSize || venturing)
                {
                    return; // her slots are full, or the odd ones wait for a full stack while she fishes
                }
            }
            if (single.ok && single.unit > static_cast<float>(vendorUnit))
            {
                while (left > 0 && list(PChar, charid, item, false, single, 1, vendorUnit, borrowed))
                {
                    --left;
                }
                return;
            }
            if (left > 0 && vendorUnit > 0)
            {
                vendor(PChar, charid, item, left, vendorUnit, borrowed);
            }
            else if (left > 0 && !venturing)
            {
                keep(charid, item, left);
            }
        }

        // Her line sold from her character as it is: loaded for it a moment
        // while she is faded, never while her player plays her
        void sellLine(const uint32 charid)
        {
            std::vector<std::pair<uint16, uint32>> line;
            const auto rset = db::preparedStmt("SELECT item, SUM(quantity) AS quantity FROM cardian_venture_items WHERE charid = ? AND `where` = ? AND quantity > 0 "
                                               "GROUP BY item",
                                               charid, kCarried);
            while (rset && rset->next())
            {
                line.emplace_back(rset->get<uint16>("item"), rset->get<uint32>("quantity"));
            }
            if (line.empty())
            {
                return;
            }
            CCharEntity* PChar = loaded(charid);
            if (played(PChar) || (PChar == nullptr && pawn::players::online(charid)))
            {
                return; // his to sell while he plays her, a zone line's moment out of memory included
            }
            std::unique_ptr<CCharEntity> borrowed;
            if (PChar == nullptr)
            {
                const auto now = timer::now();
                if (const auto it = fadedAt.find(charid); it != fadedAt.end() && now - it->second < kFadedEvery)
                {
                    return;
                }
                fadedAt[charid] = now;
                borrowed        = charutils::LoadChar(charid);
                if (borrowed == nullptr)
                {
                    return;
                }
                borrowed->clearPacketList();
                PChar = borrowed.get();
                pawn::bodies::hold(charid, PChar);
            }
            if (pawn::items::tidyStacks(PChar) > 0)
            {
                quiet(PChar, charid, borrowed != nullptr);
            }
            const auto errand    = pawn::errands::viewOf(charid);
            const bool venturing = errand.has_value() && errand->kind == cardian::errand::Kind::Money;
            for (const auto& [item, carried] : line)
            {
                const uint32 held = inInventory(PChar, item);
                if (held < carried)
                {
                    // what left her bags another way is not the line's any more
                    takeOff(charid, item, kCarried, split(charid, item, kCarried, carried - held));
                }
                if (const uint32 count = std::min(held, carried); count > 0)
                {
                    sellPiece(PChar, charid, item, count, venturing, borrowed != nullptr);
                }
            }
            if (borrowed != nullptr)
            {
                borrowed->clearPacketList();
                persist::flush(borrowed.get(), IsLogout::Yes);
                pawn::bodies::letGo(charid);
            }
        }
    } // namespace

    void ensureTables()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_listings` ("
                         "`id` int(10) unsigned NOT NULL, " // auction_house's row
                         "`charid` int(10) unsigned NOT NULL, "
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`quantity` smallint(5) unsigned NOT NULL DEFAULT 1, "
                         "`earlier` smallint(5) unsigned NOT NULL DEFAULT 0, " // how many of it an earlier venture's
                         "`price` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`posted` int(10) unsigned NOT NULL DEFAULT 0, " // real unix seconds
                         "PRIMARY KEY (`id`), KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("ALTER TABLE `cardian_venture_listings` ADD COLUMN IF NOT EXISTS `earlier` smallint(5) unsigned NOT NULL DEFAULT 0 AFTER `quantity`");
    }

    void newVenture(const uint32 charid)
    {
        db::preparedStmt("DELETE FROM cardian_venture_items WHERE charid = ? AND `where` IN (?, ?, ?)", charid, kSold, kReturned, kKept);
        struct Row
        {
            uint16 item;
            uint8  where;
            uint32 quantity;
            uint32 value;
        };
        std::vector<Row> rows;
        const auto       rset = db::preparedStmt("SELECT item, `where`, quantity, value FROM cardian_venture_items WHERE charid = ? AND earlier = 0", charid);
        while (rset && rset->next())
        {
            rows.push_back({ rset->get<uint16>("item"), rset->get<uint8>("where"), rset->get<uint32>("quantity"), rset->get<uint32>("value") });
        }
        for (const auto& r : rows)
        {
            putOn(charid, r.item, r.where, Part{ .earlier = r.quantity }, r.value);
        }
        db::preparedStmt("DELETE FROM cardian_venture_items WHERE charid = ? AND earlier = 0", charid);
        db::preparedStmt("UPDATE cardian_venture_listings SET earlier = quantity WHERE charid = ?", charid);
    }

    void tick()
    {
        const auto now = timer::now();
        if (now - tickedAt < kTickEvery)
        {
            return;
        }
        tickedAt = now;
        std::vector<uint32> charids;
        const auto          rset = db::preparedStmt("SELECT charid FROM cardian_venture_items WHERE `where` IN (?, ?) UNION SELECT charid FROM cardian_venture_listings",
                                                    kCarried, kListed);
        while (rset && rset->next())
        {
            charids.push_back(rset->get<uint32>("charid"));
        }
        for (const auto charid : charids)
        {
            settle(charid);
            sellLine(charid);
            restate(charid);
        }
    }
} // namespace pawn::sales
