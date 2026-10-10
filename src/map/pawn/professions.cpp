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

#include "cardian_link.h"
#include "club.h"
#include "errands.h"
#include "pawn.h"
#include "seats.h"

#include "common/database.h"
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
        constexpr uint32 kKeeperSilence  = 120; // real seconds since the keeper's last beat: past it, it is not running
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
        // her bags, neither worn nor up in her bazaar, as the venture keeper
        // takes her kit: her body's when she stands, her saved rows when not
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

        // A catalog kind's items, or fishing's gear for a slot: what may go in
        // one slot of her fishing set
        auto fitsSlot(const uint16 item, const uint8 slot) -> bool
        {
            if (slot == SLOT_RANGED || slot == SLOT_AMMO)
            {
                const auto rset = db::preparedStmt("SELECT 1 FROM cardian_venture_catalog WHERE item = ? AND kind = ?", item,
                                                   std::string(slot == SLOT_RANGED ? "rod" : "bait"));
                return rset && rset->next();
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
            std::array<std::array<uint16, 16>, kLastProfession + 1> sets{};
            bool                                                    any = false;
            const auto rset = db::preparedStmt("SELECT profession FROM cardian_professions WHERE charid = ?", ask.cardian);
            while (rset && rset->next())
            {
                if (const auto profession = rset->get<uint8>("profession"); profession >= CL_PROF_FISHING && profession <= kLastProfession)
                {
                    taken[profession] = true;
                    any               = true;
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
                }
                reply.more(row);
                ++count;
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // A rod of the catalog row, its price, how many she holds and its best
        // estimate at a spot
        auto rodRow(const uint32 charid, const uint16 item, const uint32 price, const uint16 zone, const uint16 area) -> cl_profession_tool
        {
            auto row  = make<cl_profession_tool>();
            row.item  = item;
            row.kind  = CL_TOOL_ROD;
            row.price = price;
            row.owned = static_cast<uint8>(std::min<uint32>(heldCount(charid, item), UINT8_MAX));
            if (const auto best = db::preparedStmt("SELECT COALESCE(MAX(gil_hour), 0) AS best FROM cardian_venture_combos "
                                                   "WHERE charid = ? AND zone = ? AND area = ? AND rod = ?",
                                                   charid, zone, area, item);
                best && best->next())
            {
                row.gilHour = best->get<uint32>("best");
            }
            return row;
        }

        // A profession's kit. For a venture at a spot: the starter rods,
        // cheapest first, then every other rod she holds. For a slot of her
        // set: what she holds that may go there
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
            const auto rset = db::preparedStmt("SELECT item, kind, price, starter FROM cardian_venture_catalog ORDER BY kind, starter DESC, price, item");
            while (rset && rset->next())
            {
                const auto item  = rset->get<uint16>("item");
                const bool rod   = rset->get<std::string>("kind") == "rod";
                const auto price = rset->get<uint32>("price");
                if (ask.which == CL_TOOLS_VENTURE)
                {
                    if (rod && (rset->get<uint8>("starter") != 0 || heldCount(ask.cardian, item) > 0))
                    {
                        send(rodRow(ask.cardian, item, price, ask.zone, ask.area));
                    }
                    continue;
                }
                if (ask.which == CL_TOOLS_SLOT && rod != (ask.slot == SLOT_RANGED))
                {
                    continue; // the ranged slot takes rods, the ammo slot baits
                }
                if (const auto held = heldCount(ask.cardian, item); held > 0)
                {
                    auto row  = make<cl_profession_tool>();
                    row.item  = item;
                    row.kind  = rod ? CL_TOOL_ROD : CL_TOOL_BAIT;
                    row.price = price;
                    row.owned = static_cast<uint8>(std::min<uint32>(held, UINT8_MAX));
                    send(row);
                }
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

        // Her spots, the keeper's rows: none yet while it first works them out
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
            uint8      count = 0;
            const auto rset  = db::preparedStmt("SELECT zone, area, name, level, gil_hour, hops FROM cardian_venture_spots WHERE charid = ? AND profession = ? "
                                                "ORDER BY hops, zone, area",
                                                ask.cardian, ask.profession);
            while (rset && rset->next() && count < UINT8_MAX)
            {
                auto row    = make<cl_venture_spot>();
                row.zone    = rset->get<uint16>("zone");
                row.area    = rset->get<uint16>("area");
                row.level   = rset->get<uint8>("level");
                row.hops    = rset->get<uint8>("hops");
                row.gilHour = rset->get<uint32>("gil_hour");
                setText(row.name, rset->get<std::string>("name"));
                reply.more(row);
                ++count;
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // The baits for one spot of hers with one rod, the best first
        void baits(CCharEntity* PChar, const cl_venture_baits& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            uint8      count = 0;
            const auto rset  = db::preparedStmt("SELECT b.bait, b.gil_hour, COALESCE(c.price, 0) AS price FROM cardian_venture_combos b "
                                                "LEFT JOIN cardian_venture_catalog c ON c.item = b.bait "
                                                "WHERE b.charid = ? AND b.zone = ? AND b.area = ? AND b.rod = ? ORDER BY b.gil_hour DESC, b.bait",
                                                ask.cardian, ask.zone, ask.area, ask.rod);
            while (rset && rset->next() && count < UINT8_MAX)
            {
                auto row    = make<cl_venture_bait>();
                row.item    = rset->get<uint16>("bait");
                row.gilHour = rset->get<uint32>("gil_hour");
                row.price   = rset->get<uint32>("price");
                row.owned   = static_cast<uint8>(std::min<uint32>(heldCount(ask.cardian, row.item), UINT8_MAX));
                reply.more(row);
                ++count;
            }
            auto done  = ask;
            done.count = count;
            reply.finish(done, CL_S_OK);
        }

        // Her last money venture, as the keeper last wrote it
        void report(CCharEntity* PChar, const cl_venture_report& ask, Reply& reply)
        {
            if (!mayTakeUp(PChar, ask.cardian))
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            auto done = ask;
            if (const auto rset = db::preparedStmt("SELECT profession, phase, waiting, zone, area, area_name, minutes, catches, skill_from, skill_to, "
                                                   "gil_earned, unsold_value, his_gil FROM cardian_venture_report WHERE charid = ?",
                                                   ask.cardian);
                rset && rset->next())
            {
                done.profession  = rset->get<uint8>("profession");
                done.phase       = rset->get<uint8>("phase");
                done.waiting     = rset->get<uint8>("waiting");
                done.zone        = rset->get<uint16>("zone");
                done.area        = rset->get<uint16>("area");
                done.minutes     = rset->get<uint32>("minutes");
                done.catches     = rset->get<uint32>("catches");
                done.skillFrom   = rset->get<uint16>("skill_from");
                done.skillTo     = rset->get<uint16>("skill_to");
                done.gilEarned   = rset->get<int32>("gil_earned");
                done.unsoldValue = rset->get<uint32>("unsold_value");
                done.hisGil      = rset->get<uint32>("his_gil");
                setText(done.areaName, rset->get<std::string>("area_name"));
            }
            uint8      count = 0;
            const auto items = db::preparedStmt("SELECT item, `where`, quantity, value FROM cardian_venture_items WHERE charid = ? ORDER BY `where`, value DESC",
                                                ask.cardian);
            while (items && items->next() && count < UINT8_MAX)
            {
                auto row     = make<cl_venture_item>();
                row.item     = items->get<uint16>("item");
                row.where    = items->get<uint8>("where");
                row.quantity = static_cast<uint16>(std::min<uint32>(items->get<uint32>("quantity"), UINT16_MAX));
                row.value    = items->get<uint32>("value");
                reply.more(row);
                ++count;
            }
            done.count = count;
            reply.finish(done, CL_S_OK);
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

        // A shortfall paid from his gil: the character he plays now, in memory;
        // offline, the one he played last, in its saved row (its body is not
        // loaded, so the row is the gil). Answers the payer, 0 when he is short
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

        // Her money venture is under way: an ask after it is over moves no gil
        auto workingNow(const uint32 charid) -> bool
        {
            const auto rset = db::preparedStmt("SELECT 1 FROM cardian_errands WHERE charid = ? AND kind = 'money' AND state = 'away'", charid);
            return rset && rset->next();
        }

        // The keeper's asks answered: paid from his gil, or short
        void answerAsks()
        {
            const auto rset = db::preparedStmt("SELECT id, cardian, accid, item, gil FROM cardian_venture_pay WHERE state = 'asked' ORDER BY id");
            while (rset && rset->next())
            {
                const auto id    = rset->get<uint32>("id");
                const auto gil   = rset->get<uint32>("gil");
                const auto payer = workingNow(rset->get<uint32>("cardian")) ? payFromHim(rset->get<uint32>("accid"), gil) : 0;
                db::preparedStmt("UPDATE cardian_venture_pay SET state = ?, payer = ? WHERE id = ? AND state = 'asked'", std::string(payer != 0 ? "paid" : "short"),
                                 payer, id);
                ShowInfoFmt("professions: {} asks {} gil of her player for item {}: {}", pawn::seats::nameOf(rset->get<uint32>("cardian")), gil,
                            rset->get<uint16>("item"), payer != 0 ? fmt::format("paid by {}", payer) : std::string("he is short"));
            }
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

    void ensureTables()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_professions` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`tool` smallint(5) unsigned NOT NULL DEFAULT 0, " // her set's tool: fishing, the rod
                         "`bait` smallint(5) unsigned NOT NULL DEFAULT 0, " // her set's bait
                         "`started` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `profession`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_catalog` ("
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`kind` enum('rod','bait') NOT NULL, "
                         "`price` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`name` varchar(64) NOT NULL DEFAULT '', "
                         "`starter` tinyint(3) unsigned NOT NULL DEFAULT 0, " // a starter rod, bought for a venture
                         "PRIMARY KEY (`item`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_spots` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`zone` smallint(5) unsigned NOT NULL, "
                         "`area` smallint(5) unsigned NOT NULL, "
                         "`name` varchar(32) NOT NULL DEFAULT '', "
                         "`level` tinyint(3) unsigned NOT NULL DEFAULT 0, "
                         "`gil_hour` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`hops` tinyint(3) unsigned NOT NULL DEFAULT 0, " // zone lines from where she stands
                         "PRIMARY KEY (`charid`, `profession`, `zone`, `area`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_state` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`state` mediumtext NOT NULL, "
                         "`phase` enum('trip','after','done') NOT NULL, "
                         "`updated` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
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
                         "`gil_earned` int(11) NOT NULL DEFAULT 0, "
                         "`unsold_value` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`his_gil` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`updated` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_items` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`item` smallint(5) unsigned NOT NULL, "
                         "`where` tinyint(3) unsigned NOT NULL, "
                         "`quantity` int(10) unsigned NOT NULL DEFAULT 0, "
                         "`value` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `item`, `where`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_keeper` ("
                         "`id` tinyint(3) unsigned NOT NULL, "
                         "`beat` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_profession_set` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`profession` tinyint(3) unsigned NOT NULL, "
                         "`slot` tinyint(3) unsigned NOT NULL, " // the equipment slot, SLOT_MAIN 0 to SLOT_BACK 15
                         "`item` smallint(5) unsigned NOT NULL, "
                         "PRIMARY KEY (`charid`, `profession`, `slot`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture_combos` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`zone` smallint(5) unsigned NOT NULL, "
                         "`area` smallint(5) unsigned NOT NULL, "
                         "`rod` smallint(5) unsigned NOT NULL, "
                         "`bait` smallint(5) unsigned NOT NULL, "
                         "`gil_hour` int(10) unsigned NOT NULL DEFAULT 0, "
                         "PRIMARY KEY (`charid`, `zone`, `area`, `rod`, `bait`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
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
        db::preparedStmt("ALTER TABLE `cardian_venture_catalog` ADD COLUMN IF NOT EXISTS `starter` tinyint(3) unsigned NOT NULL DEFAULT 0");
        db::preparedStmt("ALTER TABLE `cardian_venture_pay` ADD COLUMN IF NOT EXISTS `quantity` smallint(5) unsigned NOT NULL DEFAULT 1");
        db::preparedStmt("ALTER TABLE `cardian_venture_pay` ADD COLUMN IF NOT EXISTS `her_gil` int(10) unsigned NOT NULL DEFAULT 0");
        db::preparedStmt("ALTER TABLE `cardian_venture_spots` ADD COLUMN IF NOT EXISTS `hops` tinyint(3) unsigned NOT NULL DEFAULT 0");
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
        answerAsks();
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

    auto keeperAlive() -> bool
    {
        const auto rset = db::preparedStmt("SELECT beat FROM cardian_venture_keeper WHERE id = 1");
        return rset && rset->next() && realNow() <= rset->get<uint32>("beat") + kKeeperSilence;
    }

    auto hasProfession(const uint32 charid, const uint8 profession) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_professions WHERE charid = ? AND profession = ?", charid, profession);
        return rset && rset->next();
    }

    auto rodTakeable(const uint32 charid, const uint16 rod) -> bool
    {
        const auto rset = db::preparedStmt("SELECT starter FROM cardian_venture_catalog WHERE item = ? AND kind = 'rod'", rod);
        return rset && rset->next() && (rset->get<uint8>("starter") != 0 || heldCount(charid, rod) > 0);
    }

    void saveSet(const uint32 charid, const uint8 profession, const uint16 rod, const uint16 bait)
    {
        db::preparedStmt("UPDATE cardian_professions SET tool = ?, bait = ? WHERE charid = ? AND profession = ?", rod, bait, charid, profession);
        db::preparedStmt("REPLACE INTO cardian_profession_set (charid, profession, slot, item) VALUES (?, ?, ?, ?), (?, ?, ?, ?)", charid, profession,
                         static_cast<uint8>(SLOT_RANGED), rod, charid, profession, static_cast<uint8>(SLOT_AMMO), bait);
    }

    auto hasAnyProfession(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_professions WHERE charid = ? LIMIT 1", charid);
        return rset && rset->next();
    }

    auto spotOf(const uint32 charid, const uint8 profession, const uint16 zone, const uint16 area) -> std::optional<Spot>
    {
        const auto rset = db::preparedStmt("SELECT s.name, COALESCE(a.center_x, 0) AS x, COALESCE(a.center_y, 0) AS y, COALESCE(a.center_z, 0) AS z "
                                           "FROM cardian_venture_spots s LEFT JOIN fishing_area a ON a.zoneid = s.zone AND a.areaid = s.area "
                                           "WHERE s.charid = ? AND s.profession = ? AND s.zone = ? AND s.area = ?",
                                           charid, profession, zone, area);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        return Spot{ .zone   = zone,
                     .area   = area,
                     .name   = rset->get<std::string>("name"),
                     .centre = position_t(rset->get<float>("x"), rset->get<float>("y"), rset->get<float>("z"), 0, 0) };
    }

    auto isBait(const uint16 item) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_venture_catalog WHERE item = ? AND kind = 'bait'", item);
        return rset && rset->next();
    }

    auto hasReport(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_venture_report WHERE charid = ?", charid);
        return rset && rset->next();
    }
} // namespace pawn::professions
