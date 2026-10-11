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

#include "professions.h"

#include "angler.h"
#include "cardian_link.h"
#include "club.h"
#include "errands.h"
#include "pawn.h"
#include "seats.h"
#include "venture_shop.h"
#include "venture_spots.h"

#include "common/database.h"
#include "common/settings.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/transactions/item_claim.h"
#include "map_session.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::professions
{
    namespace
    {
        using namespace cardian::link;
        using Member = cardian::errand::Member;

        constexpr auto   kTickEvery      = std::chrono::seconds(3);
        constexpr uint8  kLastProfession = CL_PROF_COOKING;

        // The game's skill for a profession (char_skills' skillid); HELM has none
        constexpr std::array<uint8, kLastProfession + 1> kSkillOf{ 0, 48, 0, 0, 0, 0, 49, 50, 51, 52, 53, 54, 55, 56 };

        // Every bag a tool or a bait she holds may be in: her inventory and her wardrobes
        constexpr std::array<uint8, 9> kBags{ LOC_INVENTORY, LOC_WARDROBE, LOC_WARDROBE2, LOC_WARDROBE3, LOC_WARDROBE4,
                                              LOC_WARDROBE5, LOC_WARDROBE6, LOC_WARDROBE7, LOC_WARDROBE8 };

        timer::time_point tickedAt{};

        auto realNow() -> uint32
        {
            return static_cast<uint32>(std::time(nullptr));
        }

        // His alt or a cardian his account owns: who may take up a profession
        auto mayTakeUp(const CCharEntity* PPlayer, const uint32 charid) -> bool
        {
            const auto member = pawn::club::memberOf(PPlayer, charid);
            return member.has_value() && (*member == Member::Alt || *member == Member::Owned);
        }

        // She wears it
        auto worn(CCharEntity* PPawn, const CItem* PItem) -> bool
        {
            for (uint8 slot = SLOT_MAIN; slot <= SLOT_BACK; ++slot)
            {
                if (static_cast<const CItem*>(PPawn->getEquip(static_cast<SLOTTYPE>(slot))) == PItem)
                {
                    return true;
                }
            }
            return false;
        }

        // How many of an item she holds free to take on a venture -- in any of
        // her bags, neither worn nor up in her bazaar: her body's when she
        // stands, her saved rows when not
        auto heldCount(const uint32 charid, const uint16 item) -> uint32
        {
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                uint32 count = 0;
                for (const auto bag : kBags)
                {
                    auto* PBag = PPawn->getStorage(bag);
                    if (PBag == nullptr)
                    {
                        continue;
                    }
                    for (uint8 slot = 0; slot <= PBag->GetSize(); ++slot)
                    {
                        if (const auto* PItem = PBag->GetItem(slot); PItem != nullptr && PItem->getID() == item && PItem->getCharPrice() == 0 && !worn(PPawn, PItem))
                        {
                            count += PItem->getQuantity();
                        }
                    }
                }
                return count;
            }
            const auto rset = db::preparedStmt("SELECT COALESCE(SUM(i.quantity), 0) AS held FROM char_inventory i WHERE i.charid = ? AND i.itemId = ? "
                                               "AND i.location IN (0, 8, 10, 11, 12, 13, 14, 15, 16) AND i.bazaar = 0 AND NOT EXISTS "
                                               "(SELECT 1 FROM char_equip e WHERE e.charid = i.charid AND e.containerid = i.location AND e.slotid = i.slot)",
                                               charid, item);
            return rset && rset->next() ? rset->get<uint32>("held") : 0;
        }

        // Her open inventory spaces: her character's where it is in memory,
        // else her saved bag's size less what it holds
        auto freeInventory(const uint32 charid) -> uint8
        {
            const CCharEntity* PChar = pawn::angler::bodyOf(charid);
            PChar                    = PChar != nullptr ? PChar : pawn::findPawn(charid);
            PChar                    = PChar != nullptr ? PChar : zoneutils::GetChar(charid);
            if (PChar != nullptr)
            {
                return PChar->getStorage(LOC_INVENTORY)->GetFreeSlotsCount();
            }
            const auto size = db::preparedStmt("SELECT inventory FROM char_storage WHERE charid = ?", charid);
            const auto used = db::preparedStmt("SELECT COUNT(*) AS used FROM char_inventory WHERE charid = ? AND location = 0 AND slot > 0", charid);
            if (!size || !size->next() || !used || !used->next())
            {
                return 0;
            }
            const auto total = size->get<uint32>("inventory");
            const auto held  = used->get<uint32>("used");
            return static_cast<uint8>(total > held ? std::min<uint32>(total - held, UINT8_MAX) : 0);
        }

        // Her skill in a profession, in the game's tenths: her body's when she stands
        auto skillOf(const uint32 charid, const uint8 profession) -> uint16
        {
            const uint8 skill = profession <= kLastProfession ? kSkillOf[profession] : 0;
            if (skill == 0)
            {
                return 0;
            }
            if (const auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn->RealSkills.skill[skill];
            }
            const auto rset = db::preparedStmt("SELECT value FROM char_skills WHERE charid = ? AND skillid = ?", charid, skill);
            return rset && rset->next() ? rset->get<uint16>("value") : 0;
        }

        // Her guild rank in a profession, the game's craft rank: her body's
        // when she stands
        auto rankOf(const uint32 charid, const uint8 profession) -> uint8
        {
            const uint8 skill = profession <= kLastProfession ? kSkillOf[profession] : 0;
            if (skill == 0)
            {
                return 0;
            }
            if (const auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn->RealSkills.rank[skill];
            }
            const auto rset = db::preparedStmt("SELECT `rank` FROM char_skills WHERE charid = ? AND skillid = ?", charid, skill);
            return rset && rset->next() ? rset->get<uint8>("rank") : 0;
        }

        // Fishing's own gear: every piece with the game's fishing skill bonus
        // (item_mods' 127), by the slots it fits, read once
        constexpr uint16 kFishingSkillMod = 127;

        auto fishingGear() -> const std::unordered_map<uint16, uint32>&
        {
            static const auto gear = []
            {
                std::unordered_map<uint16, uint32> out;
                const auto rset = db::preparedStmt("SELECT DISTINCT e.itemId, e.slot FROM item_equipment e JOIN item_mods m ON m.itemId = e.itemId "
                                                   "WHERE m.modId = ?",
                                                   kFishingSkillMod);
                while (rset && rset->next())
                {
                    out[rset->get<uint16>("itemId")] = rset->get<uint32>("slot");
                }
                return out;
            }();
            return gear;
        }

        auto isRod(const uint16 item) -> bool
        {
            const auto rset = db::preparedStmt("SELECT 1 FROM fishing_rod WHERE rodid = ?", item);
            return rset && rset->next();
        }

        // A rod of the game's (fishing_rod) in the ranged slot, a bait or
        // lure (fishing_bait) in the ammo slot, fishing's gear in the rest:
        // what may go in one slot of her fishing set
        auto fitsSlot(const uint16 item, const uint8 slot) -> bool
        {
            if (slot == SLOT_RANGED || slot == SLOT_AMMO)
            {
                return slot == SLOT_RANGED ? isRod(item) : isBait(item);
            }
            const auto it = fishingGear().find(item);
            return it != fishingGear().end() && (it->second & (1u << slot)) != 0;
        }

        // ---- the Link ----------------------------------------------------------

        // Every profession in order, with where it stands with her, and for
        // one taken up its set. The first build lets her take up one, and only
        // fishing
        void professions(CCharEntity* PChar, const cl_professions& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            std::array<bool, kLastProfession + 1>                    taken{};
            std::array<bool, kLastProfession + 1>                    letItemsGo{};
            std::array<std::array<uint16, 16>, kLastProfession + 1> sets{};
            bool                                                    any = false;
            const auto rset = db::preparedStmt("SELECT profession, items_go FROM cardian_professions WHERE charid = ?", ask.cardian);
            while (rset && rset->next())
            {
                if (const auto profession = rset->get<uint8>("profession"); profession >= CL_PROF_FISHING && profession <= kLastProfession)
                {
                    taken[profession]      = true;
                    letItemsGo[profession] = rset->get<uint8>("items_go") != 0;
                    any                    = true;
                }
            }
            const auto slots = db::preparedStmt("SELECT profession, slot, item FROM cardian_profession_set WHERE charid = ?", ask.cardian);
            while (slots && slots->next())
            {
                const auto profession = slots->get<uint8>("profession");
                const auto slot       = slots->get<uint8>("slot");
                if (profession <= kLastProfession && slot < 16)
                {
                    sets[profession][slot] = slots->get<uint16>("item");
                }
            }
            uint8 count = 0;
            for (uint8 profession = CL_PROF_FISHING; profession <= kLastProfession; ++profession)
            {
                auto row       = make<cl_profession>();
                row.profession = profession;
                row.state      = taken[profession] ? CL_PROF_TAKEN : (profession == CL_PROF_FISHING && !any ? CL_PROF_OPEN : CL_PROF_LOCKED);
                row.skill      = skillOf(ask.cardian, profession);
                row.rank       = rankOf(ask.cardian, profession);
                if (taken[profession])
                {
                    std::copy(sets[profession].begin(), sets[profession].end(), row.set);
                    row.letItemsGo = letItemsGo[profession] ? 1 : 0;
                }
                reply.more(row);
                ++count;
            }
            auto done      = ask;
            done.count     = count;
            done.freeSlots   = freeInventory(ask.cardian);
            done.debugLength = settings::get<bool>("pawn.VENTURE_DEBUG") ? 1 : 0;
            reply.finish(done, CL_S_OK);
        }

        // A piece of her kit's row: what a venture's purchase of it costs now
        // where it costs least (`prices`, venture_shop.h; 0 when nobody sells
        // it), and how many she holds
        auto toolRow(const uint32 charid, const uint16 item, const uint8 kind, const std::unordered_map<uint16, uint32>& prices) -> cl_profession_tool
        {
            auto row  = make<cl_profession_tool>();
            row.item  = item;
            row.kind  = kind;
            row.price = prices.contains(item) ? prices.at(item) : 0;
            row.owned = static_cast<uint8>(std::min<uint32>(heldCount(charid, item), UINT8_MAX));
            return row;
        }

        // A profession's kit. For a venture: the starter rods, cheapest first,
        // then every other rod she holds. For a slot of her set: what she
        // holds that may go there
        void tools(CCharEntity* PChar, const cl_profession_tools& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (ask.profession != CL_PROF_FISHING || (ask.which != CL_TOOLS_VENTURE && ask.which != CL_TOOLS_SLOT) ||
                (ask.which == CL_TOOLS_SLOT && ask.slot > SLOT_BACK))
            {
                reply.finish(ask, CL_S_NOT_OFFERED);
                return;
            }
            uint8      count = 0;
            const auto send  = [&](const cl_profession_tool& row)
            {
                if (count < UINT8_MAX)
                {
                    reply.more(row);
                    ++count;
                }
            };
            if (ask.which == CL_TOOLS_SLOT && ask.slot != SLOT_RANGED && ask.slot != SLOT_AMMO)
            {
                // the profession's gear for the slot, what she holds of it
                for (const auto& [item, fits] : fishingGear())
                {
                    if ((fits & (1u << ask.slot)) == 0)
                    {
                        continue;
                    }
                    if (const auto held = heldCount(ask.cardian, item); held > 0)
                    {
                        auto row  = make<cl_profession_tool>();
                        row.item  = item;
                        row.kind  = CL_TOOL_GEAR;
                        row.owned = static_cast<uint8>(std::min<uint32>(held, UINT8_MAX));
                        send(row);
                    }
                }
                auto done  = ask;
                done.count = count;
                reply.finish(done, CL_S_OK);
                return;
            }
            // the starter rods, for a venture; then what she holds that may go
            const auto starters = ask.which == CL_TOOLS_VENTURE ? starterRods() : std::vector<uint16>{};
            const bool rods     = ask.which == CL_TOOLS_VENTURE || ask.slot == SLOT_RANGED;
            std::vector<uint16> held;
            const auto rset = db::preparedStmt(rods ? "SELECT rodid AS item FROM fishing_rod ORDER BY rodid" : "SELECT baitid AS item FROM fishing_bait ORDER BY baitid");
            while (rset && rset->next())
            {
                const auto item = rset->get<uint16>("item");
                if (std::ranges::find(starters, item) == starters.end() && heldCount(ask.cardian, item) > 0)
                {
                    held.push_back(item);
                }
            }
            std::vector<uint16> all = starters;
            all.insert(all.end(), held.begin(), held.end());
            const auto prices = pawn::ventureshop::purchasePrices(all);

            // the starter rods cheapest first (one nobody sells last)
            std::vector<cl_profession_tool> first;
            for (const auto rod : starters)
            {
                first.push_back(toolRow(ask.cardian, rod, CL_TOOL_ROD, prices));
            }
            std::ranges::stable_sort(first, {}, [](const cl_profession_tool& r) { return std::pair(r.price == 0, r.price); });
            for (const auto& row : first)
            {
                send(row);
            }
            for (const auto item : held)
            {
                send(toolRow(ask.cardian, item, rods ? CL_TOOL_ROD : CL_TOOL_BAIT, prices));
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // One slot of her set filled with something she holds that may go
        // there, or emptied
        void setSlot(CCharEntity* PChar, const cl_profession_slot& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (!hasProfession(ask.cardian, ask.profession))
            {
                reply.finish(ask, CL_S_NO_PROFESSION);
                return;
            }
            if (ask.profession != CL_PROF_FISHING || ask.slot > SLOT_BACK)
            {
                reply.finish(ask, CL_S_NOT_OFFERED);
                return;
            }
            if (ask.item == 0)
            {
                db::preparedStmt("DELETE FROM cardian_profession_set WHERE charid = ? AND profession = ? AND slot = ?", ask.cardian, ask.profession, ask.slot);
            }
            else
            {
                if (!fitsSlot(ask.item, ask.slot) || heldCount(ask.cardian, ask.item) == 0)
                {
                    reply.finish(ask, CL_S_NOT_A_TOOL);
                    return;
                }
                db::preparedStmt("REPLACE INTO cardian_profession_set (charid, profession, slot, item) VALUES (?, ?, ?, ?)", ask.cardian, ask.profession,
                                 ask.slot, ask.item);
            }
            reply.finish(ask, CL_S_OK);
        }

        // Take up a profession: fishing, the only one for now, her first. Its
        // set starts empty
        void start(CCharEntity* PChar, const cl_start_profession& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (ask.profession != CL_PROF_FISHING)
            {
                reply.finish(ask, CL_S_NOT_OFFERED);
                return;
            }
            if (hasAnyProfession(ask.cardian))
            {
                reply.finish(ask, CL_S_ONE_PROFESSION);
                return;
            }
            db::preparedStmt("INSERT INTO cardian_professions (charid, profession, tool, bait, started) VALUES (?, ?, 0, 0, ?)", ask.cardian, ask.profession,
                             static_cast<uint32>(earth_time::game_timestamp()));
            ShowInfoFmt("professions: {} has {} ({}) take up fishing", PChar->getName(), pawn::seats::nameOf(ask.cardian), ask.cardian);
            reply.finish(ask, CL_S_OK);
        }

        // Her waters, worked out as asked (venture_spots.h), nearest first
        void spots(CCharEntity* PChar, const cl_venture_spots& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (!hasProfession(ask.cardian, ask.profession))
            {
                reply.finish(ask, CL_S_NO_PROFESSION);
                return;
            }
            uint8 count = 0;
            for (const auto& spot : pawn::venturespots::spotsOf(ask.cardian))
            {
                if (count == UINT8_MAX)
                {
                    break;
                }
                auto row = make<cl_venture_spot>();
                row.zone = spot.zone;
                row.area = spot.area;
                row.hops = spot.hops;
                setText(row.name, spot.name);
                reply.more(row);
                ++count;
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // Every bait and lure the game has (fishing_bait), whatever the spot and
        // the rod: which works where is the player's to find (the user,
        // 2026-10-10). What she holds first, then what is for sale -- the
        // auction house or a guild shop, wherever it costs least
        // (venture_shop.h) -- cheapest first, then the rest, which nobody
        // sells now
        void baits(CCharEntity* PChar, const cl_venture_baits& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            struct Bait
            {
                uint16      item  = 0;
                uint32      price = 0;
                uint32      owned = 0;
                std::string name;
            };
            std::vector<Bait> all;
            const auto rset = db::preparedStmt("SELECT baitid, name FROM fishing_bait");
            while (rset && rset->next())
            {
                const auto item = rset->get<uint16>("baitid");
                all.push_back({ .item = item, .owned = heldCount(ask.cardian, item), .name = rset->get<std::string>("name") });
            }
            std::vector<uint16> items;
            for (const auto& b : all)
            {
                items.push_back(b.item);
            }
            const auto prices = pawn::ventureshop::purchasePrices(items);
            for (auto& b : all)
            {
                b.price = prices.contains(b.item) ? prices.at(b.item) : 0;
            }
            const auto rank = [](const Bait& b) { return b.owned > 0 ? 0 : (b.price > 0 ? 1 : 2); };
            std::ranges::sort(all, [&](const Bait& x, const Bait& y)
                              {
                                  if (rank(x) != rank(y))
                                  {
                                      return rank(x) < rank(y);
                                  }
                                  if (rank(x) == 1 && x.price != y.price)
                                  {
                                      return x.price < y.price;
                                  }
                                  return x.name < y.name;
                              });
            uint8 count = 0;
            for (const auto& b : all)
            {
                if (count == UINT8_MAX)
                {
                    break;
                }
                auto row  = make<cl_venture_bait>();
                row.item  = b.item;
                row.price = b.price;
                row.owned = static_cast<uint8>(std::min<uint32>(b.owned, UINT8_MAX));
                reply.more(row);
                ++count;
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // Her last money venture, its own: its sums, its catch by kind, and
        // each part of her line to sell, an earlier venture's marked. Each
        // answer marks the furthest phase her player has read it at (`seen`):
        // read once she is back, the page stops saying the venture completed
        // (ventureDone); read once it is done -- over, and nothing left to
        // sell -- it is cleared (hasReport)
        void report(CCharEntity* PChar, const cl_venture_report& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            auto done = ask;
            if (const auto rset = db::preparedStmt("SELECT profession, phase, zone, area, area_name, left_at, length, minutes, catches, skill_from, skill_to, "
                                                   "gil_earned, supplies, spent FROM cardian_venture_report WHERE charid = ?",
                                                   ask.cardian);
                rset && rset->next())
            {
                const uint32 now    = earth_time::game_timestamp();
                const uint32 leftAt = rset->get<uint32>("left_at");
                done.profession     = rset->get<uint8>("profession");
                done.phase          = rset->get<uint8>("phase");
                done.zone           = rset->get<uint16>("zone");
                done.area           = rset->get<uint16>("area");
                done.length         = rset->get<uint32>("length");
                // away: how long so far
                done.minutes   = done.phase == CL_VENTURE_WORKING ? std::min(done.length, now > leftAt ? (now - leftAt) / 60 : 0) : rset->get<uint32>("minutes");
                done.catches   = rset->get<uint32>("catches");
                done.skillFrom = rset->get<uint16>("skill_from");
                done.skillTo   = rset->get<uint16>("skill_to");
                done.earned    = rset->get<int32>("gil_earned");
                done.supplies  = rset->get<uint32>("supplies");
                done.spent     = rset->get<uint32>("spent");
                setText(done.areaName, rset->get<std::string>("area_name"));
            }
            if (const auto rset = db::preparedStmt("SELECT COALESCE(SUM(value), 0) AS pending FROM cardian_venture_items WHERE charid = ? AND earlier = 0 "
                                                   "AND `where` IN (?, ?)",
                                                   ask.cardian, static_cast<uint8>(CL_VENTURE_CARRIED), static_cast<uint8>(CL_VENTURE_LISTED));
                rset && rset->next())
            {
                done.pending = rset->get<uint32>("pending");
            }
            uint8      count   = 0;
            const auto caught  = db::preparedStmt("SELECT item, quantity FROM cardian_venture_catches WHERE charid = ? ORDER BY quantity DESC, item", ask.cardian);
            while (caught && caught->next() && count < UINT8_MAX)
            {
                auto row     = make<cl_venture_item>();
                row.item     = caught->get<uint16>("item");
                row.where    = CL_VENTURE_CAUGHT;
                row.quantity = static_cast<uint16>(std::min<uint32>(caught->get<uint32>("quantity"), UINT16_MAX));
                reply.more(row);
                ++count;
            }
            const auto items = db::preparedStmt("SELECT earlier, item, `where`, quantity, value FROM cardian_venture_items WHERE charid = ? "
                                                "ORDER BY earlier, `where`, value DESC",
                                                ask.cardian);
            while (items && items->next() && count < UINT8_MAX)
            {
                auto row     = make<cl_venture_item>();
                row.item     = items->get<uint16>("item");
                row.where    = items->get<uint8>("where");
                row.earlier  = items->get<uint8>("earlier");
                row.quantity = static_cast<uint16>(std::min<uint32>(items->get<uint32>("quantity"), UINT16_MAX));
                row.value    = items->get<uint32>("value");
                reply.more(row);
                ++count;
            }
            done.count = count;
            reply.finish(done, CL_S_OK);
            if (done.phase != CL_VENTURE_NONE)
            {
                db::preparedStmt("UPDATE cardian_venture_report SET seen = GREATEST(seen, ?) WHERE charid = ?", done.phase, ask.cardian);
            }
        }

        // ---- his gil -----------------------------------------------------------

        // The character the player plays now, with a client of his own
        auto playingNow(const uint32 accid) -> CCharEntity*
        {
            const auto rset = db::preparedStmt("SELECT charid FROM accounts_sessions WHERE accid = ? AND client_addr <> 0", accid);
            while (rset && rset->next())
            {
                if (auto* PChar = zoneutils::GetChar(rset->get<uint32>("charid")); PChar != nullptr && !pawn::isPawn(PChar))
                {
                    return PChar;
                }
            }
            return nullptr;
        }

        // A payment's line to the payer's addon, once it is bound
        void tellPaid()
        {
            const auto rset = db::preparedStmt("SELECT id, cardian, profession, item, quantity, her_gil, gil, payer FROM cardian_venture_pay "
                                               "WHERE state = 'paid' AND told = 0 ORDER BY id");
            while (rset && rset->next())
            {
                auto paid       = make<cl_venture_paid>();
                paid.cardian    = rset->get<uint32>("cardian");
                paid.profession = rset->get<uint8>("profession");
                paid.item       = rset->get<uint16>("item");
                paid.quantity   = rset->get<uint16>("quantity");
                paid.herGil     = rset->get<uint32>("her_gil");
                paid.hisGil     = rset->get<uint32>("gil");
                if (cardian::link::send(rset->get<uint32>("payer"), paid))
                {
                    db::preparedStmt("UPDATE cardian_venture_pay SET told = 1 WHERE id = ?", rset->get<uint32>("id"));
                }
            }
        }
        // A venture's news to his addon: the character the account plays now,
        // once it is bound; held until then
        void tellNews()
        {
            const auto rset = db::preparedStmt("SELECT id, cardian, accid, news, item FROM cardian_venture_news WHERE told = 0 ORDER BY id");
            while (rset && rset->next())
            {
                const auto* PPlayer = playingNow(rset->get<uint32>("accid"));
                if (PPlayer == nullptr)
                {
                    continue;
                }
                auto news    = make<cl_venture_news>();
                news.cardian = rset->get<uint32>("cardian");
                news.news    = rset->get<uint8>("news");
                news.item    = rset->get<uint16>("item");
                if (cardian::link::send(PPlayer->id, news))
                {
                    db::preparedStmt("UPDATE cardian_venture_news SET told = 1 WHERE id = ?", rset->get<uint32>("id"));
                }
            }
        }
    } // namespace

    auto payFromHim(const uint32 accid, const uint32 gil) -> uint32
    {
        if (auto* PPlayer = playingNow(accid); PPlayer != nullptr)
        {
            const auto* PGil = PPlayer->getStorage(LOC_INVENTORY)->GetItem(0);
            if (PGil == nullptr || !PGil->isType(ITEM_CURRENCY) || PGil->getQuantity() < gil)
            {
                return 0;
            }
            auto transaction = ItemClaimTransaction::start(PPlayer);
            return transaction && transaction->pay(gil) && transaction->commit() ? PPlayer->id : 0;
        }
        const auto last = db::preparedStmt("SELECT charid FROM cardian_last_played WHERE accid = ?", accid);
        if (!last || !last->next())
        {
            return 0;
        }
        const auto payer = last->get<uint32>("charid");
        if (zoneutils::GetChar(payer) != nullptr)
        {
            return 0; // standing as a cardian: its body holds the gil, not the row
        }
        const auto paid = db::preparedStmt("UPDATE char_inventory SET quantity = quantity - ? WHERE charid = ? AND location = 0 AND slot = 0 "
                                           "AND quantity >= ?",
                                           gil, payer, gil);
        return paid && paid->rowsAffected() == 1 ? payer : 0;
    }

    void refundHim(const uint32 payer, const uint32 gil)
    {
        if (auto* PPayer = zoneutils::GetChar(payer); PPayer != nullptr)
        {
            auto transaction = ItemClaimTransaction::start(PPayer);
            if (transaction && transaction->earn(gil) && transaction->commit())
            {
                return;
            }
        }
        else if (const auto back = db::preparedStmt("UPDATE char_inventory SET quantity = quantity + ? WHERE charid = ? AND location = 0 AND slot = 0", gil, payer);
                 back && back->rowsAffected() == 1)
        {
            return;
        }
        ShowErrorFmt("professions: {} gil could not be handed back to {}", gil, payer);
    }

    auto starterRods() -> std::vector<uint16>
    {
        std::vector<uint16> rods;
        for (const auto& part : split(settings::get<std::string>("pawn.VENTURE_STARTER_RODS"), ","))
        {
            if (const auto rod = static_cast<uint16>(std::strtoul(part.c_str(), nullptr, 10)); rod != 0)
            {
                rods.push_back(rod);
            }
        }
        return rods;
    }

    auto isStarter(const uint16 rod) -> bool
    {
        const auto rods = starterRods();
        return std::ranges::find(rods, rod) != rods.end();
    }

    void recordPurchase(const Purchase& p)
    {
        db::preparedStmt("INSERT INTO cardian_venture_pay (cardian, accid, profession, item, gil, state, payer, quantity, her_gil, asked_at) "
                         "VALUES (?, ?, ?, ?, ?, 'paid', ?, ?, ?, ?)",
                         p.cardian, p.accid, p.profession, p.item, p.hisGil, p.payer, p.quantity, p.herGil, realNow());
    }

    void recordNews(const uint32 cardian, const uint32 accid, const uint8 news, const uint16 item)
    {
        db::preparedStmt("INSERT INTO cardian_venture_news (cardian, accid, news, item, at) VALUES (?, ?, ?, ?, ?)", cardian, accid, news, item, realNow());
    }

    void ensureTables()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_professions` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`tool` smallint(5) unsigned NOT NULL DEFAULT 0, " // her set's tool: fishing, the rod
                         "`bait` smallint(5) unsigned NOT NULL DEFAULT 0, " // her set's bait
                         "`items_go` tinyint(3) unsigned NOT NULL DEFAULT 0, " // her last venture let items go
                         "`started` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `profession`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_pay` ("
                         "`id` int(10) unsigned NOT NULL AUTO_INCREMENT, "
                         "`cardian` int(10) unsigned NOT NULL, "
                         "`accid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`gil` int(10) unsigned NOT NULL, "
                         "`state` enum('asked','paid','short') NOT NULL DEFAULT 'asked', "
                         "`payer` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`told` tinyint(3) unsigned NOT NULL DEFAULT 0, "
                         "`credited` tinyint(3) unsigned NOT NULL DEFAULT 0, "
                         "`asked_at` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`quantity` smallint(5) unsigned NOT NULL DEFAULT 1, "
                         "`her_gil` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`id`), KEY (`cardian`), KEY (`state`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_report` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`phase` tinyint(3) unsigned NOT NULL, "
                         "`waiting` tinyint(3) unsigned NOT NULL DEFAULT 0, "
                         "`zone` smallint(5) unsigned NOT NULL DEFAULT 0, "
                         "`area` smallint(5) unsigned NOT NULL DEFAULT 0, "
                         "`area_name` varchar(32) NOT NULL DEFAULT '', "
                         "`minutes` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`catches` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`skill_from` smallint(5) unsigned NOT NULL DEFAULT 0, "
                         "`skill_to` smallint(5) unsigned NOT NULL DEFAULT 0, "
                         "`gil_earned` int(11) NOT NULL DEFAULT 0, " // what this venture's catch has fetched, less the auction house's fees
                         "`unsold_value` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`his_gil` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`updated` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`left_at` int(10) unsigned NOT NULL DEFAULT 0, "  // the game clock as she left
                         "`length` int(10) unsigned NOT NULL DEFAULT 0, "   // the minutes she was sent for
                         "`supplies` int(10) unsigned NOT NULL DEFAULT 0, " // what the venture used up, at its worth
                         "`spent` int(10) unsigned NOT NULL DEFAULT 0, "    // the gil paid for its supplies, hers and his
                         "`seen` tinyint(3) unsigned NOT NULL DEFAULT 0, "  // the furthest phase her player has read it at (CL_VENTURE_*)
                         "PRIMARY KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // a water's whole name, the longest 33 ("Interior of Hidden Door - Mythril")
        db::preparedStmt("ALTER TABLE `cardian_venture_report` MODIFY `area_name` varchar(64) NOT NULL DEFAULT ''");
        for (const auto* column : { "`left_at` int(10) unsigned NOT NULL DEFAULT 0", "`length` int(10) unsigned NOT NULL DEFAULT 0",
                                    "`supplies` int(10) unsigned NOT NULL DEFAULT 0", "`spent` int(10) unsigned NOT NULL DEFAULT 0",
                                    "`seen` tinyint(3) unsigned NOT NULL DEFAULT 0" })
        {
            db::preparedStmt(fmt::format("ALTER TABLE `cardian_venture_report` ADD COLUMN IF NOT EXISTS {}", column));
        }
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_items` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`earlier` tinyint(3) unsigned NOT NULL DEFAULT 0, " // 1: left from an earlier venture
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`where` tinyint(3) unsigned NOT NULL, "
                         "`quantity` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`value` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `earlier`, `item`, `where`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("ALTER TABLE `cardian_venture_items` ADD COLUMN IF NOT EXISTS `earlier` tinyint(3) unsigned NOT NULL DEFAULT 0 AFTER `charid`");
        if (const auto rset = db::preparedStmt("SELECT COUNT(*) AS keyed FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA = DATABASE() "
                                               "AND TABLE_NAME = 'cardian_venture_items' AND CONSTRAINT_NAME = 'PRIMARY' AND COLUMN_NAME = 'earlier'");
            rset && rset->next() && rset->get<uint32>("keyed") == 0)
        {
            db::preparedStmt("ALTER TABLE `cardian_venture_items` DROP PRIMARY KEY, ADD PRIMARY KEY (`charid`, `earlier`, `item`, `where`)");
        }
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_catches` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`quantity` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_profession_set` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`slot` tinyint(3) unsigned NOT NULL, " // the equipment slot, SLOT_MAIN 0 to SLOT_BACK 15
                         "`item` smallint(5) unsigned NOT NULL, "
                         "PRIMARY KEY (`charid`, `profession`, `slot`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_news` ("
                         "`id` int(10) unsigned NOT NULL AUTO_INCREMENT, "
                         "`cardian` int(10) unsigned NOT NULL, "
                         "`accid` int(10) unsigned NOT NULL, "
                         "`news` tinyint(3) unsigned NOT NULL, "
                         "`item` smallint(5) unsigned NOT NULL DEFAULT 0, "
                         "`told` tinyint(3) unsigned NOT NULL DEFAULT 0, "
                         "`at` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`id`), KEY (`told`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // an older table: her set's bait, the starter rods, a purchase's shares
        db::preparedStmt("ALTER TABLE `cardian_professions` ADD COLUMN IF NOT EXISTS `bait` smallint(5) unsigned NOT NULL DEFAULT 0");
        db::preparedStmt("ALTER TABLE `cardian_professions` ADD COLUMN IF NOT EXISTS `items_go` tinyint(3) unsigned NOT NULL DEFAULT 0");
        db::preparedStmt("ALTER TABLE `cardian_venture_pay` ADD COLUMN IF NOT EXISTS `quantity` smallint(5) unsigned NOT NULL DEFAULT 1");
        db::preparedStmt("ALTER TABLE `cardian_venture_pay` ADD COLUMN IF NOT EXISTS `her_gil` int(10) unsigned NOT NULL DEFAULT 0");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_last_played` ("
                         "`accid` int(10) unsigned NOT NULL, " // the account
                         "`charid` int(10) unsigned NOT NULL, " // the character it played last, a client of its own
                         "`at` int(10) unsigned NOT NULL DEFAULT 0, " // real unix seconds
                         "PRIMARY KEY (`accid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    }

    void registerHandlers()
    {
        cardian::link::handle<cl_professions>(professions);
        cardian::link::handle<cl_profession_tools>(tools);
        cardian::link::handle<cl_start_profession>(start);
        cardian::link::handle<cl_profession_slot>(setSlot);
        cardian::link::handle<cl_venture_spots>(spots);
        cardian::link::handle<cl_venture_baits>(baits);
        cardian::link::handle<cl_venture_report>(report);
    }

    void tick()
    {
        const auto now = timer::now();
        if (now - tickedAt < kTickEvery)
        {
            return;
        }
        tickedAt = now;
        tellPaid();
        tellNews();
    }

    void zonedIn(CCharEntity* PChar)
    {
        if (PChar == nullptr || pawn::isPawn(PChar) || PChar->PSession == nullptr)
        {
            return;
        }
        db::preparedStmt("REPLACE INTO cardian_last_played (accid, charid, at) VALUES (?, ?, ?)", pawn::ownerAccountOf(PChar), PChar->id, realNow());
    }

    auto hasProfession(const uint32 charid, const uint8 profession) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_professions WHERE charid = ? AND profession = ?", charid, profession);
        return rset && rset->next();
    }

    auto rodTakeable(const uint32 charid, const uint16 rod) -> bool
    {
        return isRod(rod) && (isStarter(rod) || heldCount(charid, rod) > 0);
    }

    void saveSet(const uint32 charid, const uint8 profession, const uint16 rod, const uint16 bait, const bool letItemsGo)
    {
        db::preparedStmt("UPDATE cardian_professions SET tool = ?, bait = ?, items_go = ? WHERE charid = ? AND profession = ?", rod, bait,
                         static_cast<uint8>(letItemsGo ? 1 : 0), charid, profession);
        db::preparedStmt("REPLACE INTO cardian_profession_set (charid, profession, slot, item) VALUES (?, ?, ?, ?), (?, ?, ?, ?)", charid, profession,
                         static_cast<uint8>(SLOT_RANGED), rod, charid, profession, static_cast<uint8>(SLOT_AMMO), bait);
    }

    auto hasAnyProfession(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_professions WHERE charid = ? LIMIT 1", charid);
        return rset && rset->next();
    }

    auto isBait(const uint16 item) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM fishing_bait WHERE baitid = ?", item);
        return rset && rset->next();
    }

    auto hasReport(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_venture_report WHERE charid = ? AND seen < ?", charid, static_cast<uint8>(CL_VENTURE_DONE));
        return rset && rset->next();
    }

    auto ventureDone(const uint32 charid) -> uint8
    {
        const auto rset = db::preparedStmt("SELECT profession FROM cardian_venture_report WHERE charid = ? AND phase IN (?, ?) AND seen < ?", charid,
                                           static_cast<uint8>(CL_VENTURE_SELLING), static_cast<uint8>(CL_VENTURE_DONE), static_cast<uint8>(CL_VENTURE_SELLING));
        return rset && rset->next() ? rset->get<uint8>("profession") : 0;
    }
} // namespace pawn::professions
