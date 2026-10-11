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

#include "angler.h"

#include "bodies.h"
#include "cardian_link_protocol.h"
#include "fish_fight.h"
#include "pawn.h"
#include "pawn_items.h"
#include "professions.h"
#include "seats.h"
#include "venture_sales.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "items/item_equipment.h"
#include "items/transactions/item_claim.h"
#include "packets/c2s/0x110_fishing_2.h"
#include "persist_batch.h"
#include "utils/charutils.h"
#include "utils/fishingutils.h"
#include "utils/itemutils.h"
#include "utils/zoneutils.h"

#include <array>
#include <ctime>
#include <memory>
#include <optional>
#include <unordered_map>

namespace pawn::angler
{
    namespace
    {
        using Mode = GP_CLI_COMMAND_FISHING_2_MODE;

        constexpr uint32 kLanded  = 0;   // the client's word ending a fight: the fish is landed
        constexpr uint32 kGaveUp  = 200; // ... the fight given up
        constexpr uint32 kGotAway = 300; // ... the fish got away
        constexpr int    kTurboCasts = 2; // the debug venture's casts a second

        // Where her rods and bait are looked for and kept: the Mog Wardrobes in
        // order, her inventory last (kept for the catch)
        constexpr std::array<uint8, 9> kKitBags{ LOC_WARDROBE, LOC_WARDROBE2, LOC_WARDROBE3, LOC_WARDROBE4, LOC_WARDROBE5,
                                                 LOC_WARDROBE6, LOC_WARDROBE7, LOC_WARDROBE8, LOC_INVENTORY };

        // A cast's steps: ready to cast, waiting for the bite, in the fight
        enum class Step : uint8
        {
            Ready,
            Biting,
            Fighting,
        };

        struct Angler
        {
            Trip                         trip;
            std::unique_ptr<CCharEntity> body;
            Step                         step    = Step::Ready;
            uint32                       at      = 0;    // the calendar's second her next step comes
            bool                         wins    = true; // the fight under way is hers
            uint16                       catchId = 0;    // what is on the line
            uint32                       rodsHeld = 0;   // her rods of the trip's kind as the cast began: one fewer after it, one broke
            uint32                       baitHeld = 0;   // ... her bait: what is gone after it was used up
        };

        // Leaked on purpose: a body still held as the process exits would be
        // destroyed after what a character's destructor needs
        auto anglers() -> std::unordered_map<uint32, Angler>&
        {
            static auto* list = new std::unordered_map<uint32, Angler>();
            return *list;
        }

        // The calendar the fishing code reads (vanadiel_timestamp): a pause
        // holds it, and her steps with it
        auto calendarNow() -> uint32
        {
            return earth_time::vanadiel_timestamp();
        }

        auto realNow() -> uint32
        {
            return static_cast<uint32>(std::time(nullptr));
        }

        auto tuning() -> cardian::fishfight::Tuning
        {
            return { .arrowSeconds = settings::get<float>("pawn.VENTURE_ARROW_SECONDS"),
                     .slowdown     = settings::get<float>("pawn.VENTURE_FISH_SLOWDOWN") };
        }

        auto nameOf(const uint32 charid) -> std::string
        {
            return pawn::seats::nameOf(charid);
        }

        auto fishingSkill(const CCharEntity* PBody) -> uint16
        {
            return PBody->RealSkills.skill[static_cast<uint8>(xi::SkillType::Fishing)];
        }

        // Her skill as a level, as the game shows it: the value is in tenths
        auto skillText(const CCharEntity* PBody) -> std::string
        {
            const auto skill = fishingSkill(PBody);
            return fmt::format("{}.{}", skill / 10, skill % 10);
        }

        // Her character as the database has it, placed in no zone; none while
        // she stands as a cardian or is played
        auto loadBody(const uint32 charid) -> std::unique_ptr<CCharEntity>
        {
            if (pawn::findPawn(charid) != nullptr || zoneutils::GetChar(charid) != nullptr)
            {
                ShowWarningFmt("angler: {} stands or is played; her body is not loaded", nameOf(charid));
                return nullptr;
            }
            auto PBody = charutils::LoadChar(charid);
            if (PBody == nullptr)
            {
                ShowErrorFmt("angler: LoadChar failed for {}", charid);
                return nullptr;
            }
            PBody->clearPacketList();
            return PBody;
        }

        // What changed on her written now: a zone's sweep never reaches a body
        // in no zone. Her equipment goes by itself: a batch written while her
        // stand's despawn flush is still marked drops her rows
        void save(CCharEntity* PBody)
        {
            if ((PBody->persist() & CharPersist::Equip) != CharPersist::None)
            {
                charutils::SaveCharEquip(PBody);
            }
            PersistBatch batch;
            batch.add(PBody);
            batch.write();
        }

        auto gilOf(CCharEntity* PBody) -> uint32
        {
            const auto* PGil = PBody->getStorage(LOC_INVENTORY)->GetItem(0);
            return PGil != nullptr && PGil->isType(ITEM_CURRENCY) ? PGil->getQuantity() : 0;
        }

        // How many of an item her inventory holds: a catch lands there
        auto inInventory(CCharEntity* PBody, const uint16 item) -> uint32
        {
            auto*  storage = PBody->getStorage(LOC_INVENTORY);
            uint32 count   = 0;
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == item)
                {
                    count += PItem->getQuantity();
                }
            }
            return count;
        }

        // How many of an item her kit bags hold, worn or not
        auto countHeld(CCharEntity* PBody, const uint16 item) -> uint32
        {
            uint32 count = 0;
            for (const uint8 loc : kKitBags)
            {
                auto* storage = PBody->getStorage(loc);
                if (storage == nullptr || !pawn::items::usableContainer(PBody, loc))
                {
                    continue;
                }
                for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
                {
                    if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == item)
                    {
                        count += PItem->getQuantity();
                    }
                }
            }
            return count;
        }

        // A piece of hers in her kit bags, wardrobes first, not in her bazaar
        struct Held
        {
            uint8 location = 0;
            uint8 slot     = 0;
        };
        auto findHeld(CCharEntity* PBody, const uint16 item) -> std::optional<Held>
        {
            for (const uint8 loc : kKitBags)
            {
                auto* storage = PBody->getStorage(loc);
                if (storage == nullptr || !pawn::items::usableContainer(PBody, loc))
                {
                    continue;
                }
                for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
                {
                    if (const auto* PItem = storage->GetItem(slot); PItem != nullptr && PItem->getID() == item && PItem->getCharPrice() == 0)
                    {
                        return Held{ loc, slot };
                    }
                }
            }
            return std::nullopt;
        }

        // Where a piece of her kit is kept: the first wardrobe with a free
        // slot, else her inventory
        auto homeOf(CCharEntity* PBody) -> uint8
        {
            for (const uint8 loc : kKitBags)
            {
                auto* storage = PBody->getStorage(loc);
                if (loc != LOC_INVENTORY && storage != nullptr && pawn::items::usableContainer(PBody, loc) && storage->GetFreeSlotsCount() > 0)
                {
                    return loc;
                }
            }
            return LOC_INVENTORY;
        }

        // Every stack of an item in her inventory moved to a wardrobe with
        // room, while one has
        void tidy(CCharEntity* PBody, const uint16 item)
        {
            auto* storage = PBody->getStorage(LOC_INVENTORY);
            for (uint8 slot = 1; slot <= storage->GetSize(); ++slot)
            {
                const auto* PItem = storage->GetItem(slot);
                if (PItem == nullptr || PItem->getID() != item || PItem->getCharPrice() != 0)
                {
                    continue;
                }
                const uint8 home = homeOf(PBody);
                if (home == LOC_INVENTORY || pawn::items::moveItem(PBody, LOC_INVENTORY, slot, home, PItem->getQuantity()) != CL_S_OK)
                {
                    return;
                }
            }
        }

        // A piece of hers worn in its slot from wherever it is; `tidied`
        // moves a kit piece out of her inventory first
        auto wear(CCharEntity* PBody, const uint16 item, const uint8 equipSlot, const bool tidied) -> bool
        {
            if (tidied)
            {
                tidy(PBody, item);
            }
            const auto held = findHeld(PBody, item);
            return held.has_value() && pawn::items::equip(PBody, held->slot, equipSlot, held->location) == CL_S_OK;
        }

        auto wears(CCharEntity* PBody, const uint8 equipSlot, const uint16 item) -> bool
        {
            const auto* PItem = PBody->getEquip(static_cast<SLOTTYPE>(equipSlot));
            return PItem != nullptr && PItem->getID() == item;
        }

        // ---- the report -------------------------------------------------------

        // A new venture's report, its own from nothing: what an earlier one
        // left to sell stays her line, as the earlier venture's
        // (venture_sales.h)
        void reportBegin(const Angler& a)
        {
            const auto& t     = a.trip;
            const auto  skill = fishingSkill(a.body.get());
            db::preparedStmt("REPLACE INTO cardian_venture_report (charid, profession, phase, zone, area, area_name, left_at, length, minutes, catches, skill_from, "
                             "skill_to, gil_earned, supplies, spent, seen, updated) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, ?, ?, 0, 0, 0, 0, ?)",
                             t.charid, t.profession, static_cast<uint8>(CL_VENTURE_WORKING), t.zone, t.area, t.areaName, t.leftAt, t.minutes, skill, skill,
                             realNow());
            db::preparedStmt("DELETE FROM cardian_venture_catches WHERE charid = ?", t.charid);
            pawn::sales::newVenture(t.charid);
        }

        // A cast done: her skill, and what landed in her bags -- her catch by
        // kind, and her line to sell, at what a vendor pays for it until her
        // seller weighs it
        void reportCast(const Angler& a, const uint16 item, const uint32 landed)
        {
            const auto&  t     = a.trip;
            const auto*  PKind = landed > 0 ? xi::items::lookup(item) : nullptr;
            const uint32 value = PKind != nullptr ? PKind->getBasePrice() * landed : 0;
            db::preparedStmt("UPDATE cardian_venture_report SET skill_to = ?, catches = catches + ?, updated = ? WHERE charid = ?", fishingSkill(a.body.get()), landed,
                             realNow(), t.charid);
            if (landed > 0)
            {
                db::preparedStmt("INSERT INTO cardian_venture_catches (charid, item, quantity) VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)",
                                 t.charid, item, landed);
                db::preparedStmt("INSERT INTO cardian_venture_items (charid, earlier, item, `where`, quantity, value) VALUES (?, 0, ?, ?, ?, ?) "
                                 "ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity), value = value + VALUES(value)",
                                 t.charid, item, static_cast<uint8>(CL_VENTURE_CARRIED), landed, value);
            }
        }

        // Gil paid for her supplies, hers and his
        void reportSpent(const uint32 charid, const uint32 gil)
        {
            db::preparedStmt("UPDATE cardian_venture_report SET spent = spent + ?, updated = ? WHERE charid = ?", gil, realNow(), charid);
        }

        // What a piece of her kit is worth: the catalog's price, else what a
        // vendor would give for it
        auto worth(const uint16 item) -> uint32
        {
            if (const auto price = pawn::professions::priceOf(item); price.has_value())
            {
                return *price;
            }
            const auto* PKind = xi::items::lookup(item);
            return PKind != nullptr ? PKind->getBasePrice() : 0;
        }

        // A cast's supplies used up: the bait gone and a rod broken, at their
        // worth, whoever bought them
        void reportUsed(const Angler& a)
        {
            auto*        PBody = a.body.get();
            const uint32 rods  = countHeld(PBody, a.trip.rod);
            const uint32 bait  = countHeld(PBody, a.trip.bait);
            const uint32 used  = (a.rodsHeld > rods ? (a.rodsHeld - rods) * worth(a.trip.rod) : 0) + (a.baitHeld > bait ? (a.baitHeld - bait) * worth(a.trip.bait) : 0);
            if (used > 0)
            {
                db::preparedStmt("UPDATE cardian_venture_report SET supplies = supplies + ?, updated = ? WHERE charid = ?", used, realNow(), a.trip.charid);
            }
        }

        void news(const Angler& a, const uint8 what, const uint16 item)
        {
            pawn::professions::recordNews(a.trip.charid, a.trip.accid, what, item);
        }

        // ---- her kit ----------------------------------------------------------

        // Bought at the catalog's price into her kit bags, her gil first and
        // then his. False when the two purses cannot pay
        auto buy(Angler& a, const uint16 item, const uint32 price, const uint32 quantity) -> bool
        {
            auto*        PBody = a.body.get();
            const uint32 cost  = price * quantity;
            const uint32 hers  = std::min(gilOf(PBody), cost);
            const uint32 his   = cost - hers;
            uint32       payer = 0;
            if (his > 0 && (payer = pawn::professions::payFromHim(a.trip.accid, his)) == 0)
            {
                ShowInfoFmt("angler: {} cannot buy item {} x{} ({} gil): hers holds {}, and her player's is short", nameOf(a.trip.charid), item, quantity, cost,
                            hers);
                return false;
            }
            auto       transaction = ItemClaimTransaction::start(PBody);
            const bool bought      = transaction && (hers == 0 || transaction->pay(hers)) && transaction->give(homeOf(PBody), item, quantity).has_value() &&
                                transaction->commit();
            PBody->clearPacketList();
            if (!bought)
            {
                ShowErrorFmt("angler: {}'s purchase of item {} x{} did not go through", nameOf(a.trip.charid), item, quantity);
                if (payer != 0)
                {
                    pawn::professions::refundHim(payer, his);
                }
                return false;
            }
            if (his > 0)
            {
                pawn::professions::recordPurchase({ .cardian    = a.trip.charid,
                                                    .accid      = a.trip.accid,
                                                    .profession = a.trip.profession,
                                                    .item       = item,
                                                    .quantity   = static_cast<uint16>(quantity),
                                                    .herGil     = hers,
                                                    .hisGil     = his,
                                                    .payer      = payer });
            }
            reportSpent(a.trip.charid, cost);
            ShowInfoFmt("angler: {} buys item {} x{} for {} gil ({} hers, {} her player's)", nameOf(a.trip.charid), item, quantity, cost, hers, his);
            return true;
        }

        // Her rod worn as she sets out: the one she wears, one of hers, or a
        // starter rod bought. A rod of hers gone since she was sent is not
        // bought with his gil
        auto kitRod(Angler& a) -> Stop
        {
            auto*        PBody = a.body.get();
            const uint16 rod   = a.trip.rod;
            if (wears(PBody, SLOT_RANGED, rod) || wear(PBody, rod, SLOT_RANGED, true))
            {
                return Stop::None;
            }
            const auto price = pawn::professions::priceOf(rod);
            if (!pawn::professions::isStarter(rod) || !price.has_value())
            {
                ShowWarningFmt("angler: {} holds no rod {} and it is not one a venture buys", nameOf(a.trip.charid), rod);
                news(a, CL_NEWS_NO_KIT, rod);
                return Stop::CannotFish;
            }
            if (buy(a, rod, *price, 1) && wear(PBody, rod, SLOT_RANGED, true))
            {
                return Stop::None;
            }
            news(a, CL_NEWS_NO_GIL, rod);
            return Stop::NoGil;
        }

        // Her bait worn: the stack she wears, more of hers, or a whole stack
        // bought, as the auction house sells it (a lure, one)
        auto kitBait(Angler& a) -> Stop
        {
            auto*        PBody = a.body.get();
            const uint16 bait  = a.trip.bait;
            if (wears(PBody, SLOT_AMMO, bait) || wear(PBody, bait, SLOT_AMMO, true))
            {
                return Stop::None;
            }
            const auto  price = pawn::professions::priceOf(bait);
            const auto* PKind = xi::items::lookup(bait);
            if (!price.has_value() || PKind == nullptr)
            {
                ShowWarningFmt("angler: {} has no bait {} left, and the auction house sells none", nameOf(a.trip.charid), bait);
                news(a, CL_NEWS_NO_KIT, bait);
                return Stop::CannotFish;
            }
            if (buy(a, bait, *price, std::max<uint32>(PKind->getStackSize(), 1)) && wear(PBody, bait, SLOT_AMMO, true))
            {
                return Stop::None;
            }
            news(a, CL_NEWS_NO_GIL, bait);
            return Stop::NoGil;
        }

        // ---- a cast -----------------------------------------------------------

        // The cast is over: the release, which is where the server raises her
        // skill, her body saved, the cast in the report, and she casts again
        // once a person would have: the casting and reeling animations, the
        // client's, take pawn.VENTURE_CAST_SECONDS of every cast
        void release(Angler& a, const uint32 now, const uint16 item, const uint32 landed)
        {
            auto* PBody = a.body.get();
            fishingutils::FishingAction(PBody, Mode::RequestRelease, 0, 0);
            PBody->clearPacketList();
            save(PBody);
            reportCast(a, item, landed);
            reportUsed(a);
            a.step = Step::Ready;
            a.at   = now + settings::get<uint32>("pawn.VENTURE_CAST_SECONDS");
        }

        // Ready: room for a catch, her rod (a spare of hers after a break),
        // her bait, and the server's leave to cast again
        auto cast(Angler& a, const uint32 now) -> Stop
        {
            auto* PBody = a.body.get();
            if (PBody->getStorage(LOC_INVENTORY)->GetFreeSlotsCount() == 0)
            {
                news(a, CL_NEWS_BAGS_FULL, 0);
                return Stop::BagsFull;
            }
            if (!wears(PBody, SLOT_RANGED, a.trip.rod))
            {
                if (!wear(PBody, a.trip.rod, SLOT_RANGED, true))
                {
                    news(a, CL_NEWS_ROD_BROKE, a.trip.rod);
                    return Stop::RodBroke;
                }
                news(a, CL_NEWS_ROD_SPARE, a.trip.rod);
            }
            if (const auto stop = kitBait(a); stop != Stop::None)
            {
                return stop;
            }
            PBody->clearPacketList();
            if (PBody->nextFishTime > now)
            {
                a.at = PBody->nextFishTime;
                return Stop::None;
            }
            a.rodsHeld = countHeld(PBody, a.trip.rod);
            a.baitHeld = countHeld(PBody, a.trip.bait);
            fishingutils::StartFishing(PBody);
            PBody->clearPacketList();
            if (PBody->animation != xi::Animation::NewFishingStart)
            {
                ShowWarningFmt("angler: {} could not cast in zone {} (area {})", nameOf(a.trip.charid), a.trip.zone, a.trip.area);
                news(a, CL_NEWS_CANNOT_FISH, 0);
                return Stop::CannotFish;
            }
            a.step = Step::Biting;
            a.at   = PBody->lastCastTime + PBody->hookDelay;
            return Stop::None;
        }

        // The bite's wait over: what the hook check hooks. Nothing, and the
        // cast is over; a monster or a chest, let go, and an item where the
        // player chose so; a fish or a thing, the fight timed
        void bite(Angler& a, const uint32 now)
        {
            auto* PBody = a.body.get();
            fishingutils::FishingAction(PBody, Mode::RequestCheckHook, 0, 0);
            PBody->clearPacketList();
            const auto* PHooked = PBody->hookedFish;
            if (PBody->animation != xi::Animation::NewFishingFish || PHooked == nullptr || !PHooked->hooked)
            {
                release(a, now, 0, 0);
                return;
            }
            if (PHooked->catchtype == FISHINGCATCHTYPE_MOB || PHooked->catchtype == FISHINGCATCHTYPE_CHEST ||
                (PHooked->catchtype == FISHINGCATCHTYPE_ITEM && a.trip.letItemsGo))
            {
                ShowInfoFmt("angler: {} lets {} go", nameOf(a.trip.charid),
                            PHooked->catchtype == FISHINGCATCHTYPE_MOB     ? std::string("a monster")
                            : PHooked->catchtype == FISHINGCATCHTYPE_CHEST ? std::string("a treasure chest")
                                                                           : fmt::format("item {}", PHooked->catchid));
                fishingutils::FishingAction(PBody, Mode::RequestEndMiniGame, kGaveUp, 0);
                PBody->clearPacketList();
                release(a, now, 0, 0);
                return;
            }
            const auto fought = cardian::fishfight::fight({ .stamina   = PHooked->stamina,
                                                            .attack    = PHooked->attackdmg,
                                                            .regen     = PHooked->regen,
                                                            .timeLimit = PHooked->timelimit,
                                                            .large     = PHooked->catchsizeType == FISHINGSIZETYPE_LARGE && a.trip.rod != EBISU },
                                                          tuning());
            ShowInfoFmt("angler: {} hooks item {} (catch level {}, stamina {}, {} an arrow, regen {}, {}s to land it): {} in {}s", nameOf(a.trip.charid),
                        PHooked->catchid, PHooked->catchlevel, PHooked->stamina, PHooked->attackdmg, PHooked->regen, PHooked->timelimit,
                        fought.won ? "landed" : "it gets away", fought.seconds);
            a.wins    = fought.won;
            a.catchId = static_cast<uint16>(PHooked->catchid);
            a.step    = Step::Fighting;
            a.at      = now + fought.seconds;
        }

        void land(Angler& a, uint32 now);

        // The dev profile's debug venture: a whole cast at once. The server's
        // own waits -- its gap between casts, the bite's wait -- are taken out
        // of her body's own clock before it checks them, and the fight and the
        // animations are not waited out; every bite, catch, break and skill-up
        // is still the fishing code's
        auto turboCast(Angler& a, const uint32 now) -> Stop
        {
            auto* PBody         = a.body.get();
            PBody->nextFishTime = 0;
            if (const auto stop = cast(a, now); stop != Stop::None || a.step != Step::Biting)
            {
                return stop;
            }
            PBody->lastCastTime = now > PBody->hookDelay ? now - PBody->hookDelay : 0;
            bite(a, now);
            if (a.step == Step::Fighting)
            {
                land(a, now);
            }
            a.at = now;
            return Stop::None;
        }

        // The fight's end as her client would tell it: landed, or got away.
        // The server's own reel check still decides a landed fish's fate
        void land(Angler& a, const uint32 now)
        {
            auto*        PBody   = a.body.get();
            const auto*  PHooked = PBody->hookedFish;
            const uint32 special = PHooked != nullptr ? PHooked->special : 0;
            const uint32 before  = inInventory(PBody, a.catchId);
            fishingutils::FishingAction(PBody, Mode::RequestEndMiniGame, a.wins ? kLanded : kGotAway, a.wins ? special : 0);
            PBody->clearPacketList();
            const uint32 after       = inInventory(PBody, a.catchId);
            const auto   catchType   = PHooked != nullptr ? PHooked->catchtype : 0;
            const auto   successType = PHooked != nullptr ? PHooked->successtype : 0;
            const auto   catchLevel  = PHooked != nullptr ? PHooked->catchlevel : 0;
            const auto   skillBefore = fishingSkill(PBody);
            release(a, now, a.catchId, after > before ? after - before : 0);
            // each catch lands in a slot of its own: her bag sorted, as a
            // client's auto-sort would, so her catch keeps to whole stacks
            if (pawn::items::tidyStacks(PBody) > 0)
            {
                PBody->clearPacketList();
            }
            ShowInfoFmt("angler: {}'s catch of item {} ends (catch type {}, outcome {}, level {}): {} landed, skill {}.{} -> {}", nameOf(a.trip.charid),
                        a.catchId, catchType, successType, catchLevel, after > before ? after - before : 0, skillBefore / 10, skillBefore % 10, skillText(PBody));
        }
    } // namespace

    auto start(const Trip& trip, Worn& worn, const std::function<void(const Worn&)>& remember) -> Stop
    {
        auto& list = anglers();
        if (list.contains(trip.charid))
        {
            return Stop::None;
        }
        auto PBody = loadBody(trip.charid);
        if (PBody == nullptr)
        {
            pawn::professions::recordNews(trip.charid, trip.accid, CL_NEWS_CANNOT_FISH, 0);
            return Stop::CannotFish;
        }
        PBody->loc.zone        = nullptr;
        PBody->loc.destination = static_cast<xi::ZoneId>(trip.zone);
        PBody->loc.p           = trip.spot;

        auto& a = list[trip.charid];
        a.trip  = trip;
        a.body  = std::move(PBody);
        auto* PHer = a.body.get();
        pawn::bodies::hold(trip.charid, PHer);

        const bool first = !worn.known;
        if (first)
        {
            const auto* PRanged = PHer->getEquip(SLOT_RANGED);
            const auto* PAmmo   = PHer->getEquip(SLOT_AMMO);
            worn = Worn{ .known = true, .ranged = PRanged != nullptr ? PRanged->getID() : uint16{ 0 }, .ammo = PAmmo != nullptr ? PAmmo->getID() : uint16{ 0 } };
            reportBegin(a);
            remember(worn);
        }
        tidy(PHer, trip.rod);
        tidy(PHer, trip.bait);
        auto stop = kitRod(a);
        if (stop == Stop::None)
        {
            stop = kitBait(a);
        }
        PHer->clearPacketList();
        save(PHer);
        if (stop == Stop::None)
        {
            ShowInfoFmt("angler: {} {} fishing in zone {} (area {}) with rod {} and bait {}, skill {}", nameOf(trip.charid), first ? "starts" : "goes on",
                        trip.zone, trip.area, trip.rod, trip.bait, skillText(PHer));
        }
        return stop;
    }

    auto fishing(const uint32 charid) -> bool
    {
        return anglers().contains(charid);
    }

    auto bodyOf(const uint32 charid) -> CCharEntity*
    {
        const auto it = anglers().find(charid);
        return it != anglers().end() ? it->second.body.get() : nullptr;
    }

    auto tick(const uint32 charid) -> Stop
    {
        auto&      list = anglers();
        const auto it   = list.find(charid);
        if (it == list.end())
        {
            return Stop::None;
        }
        auto&        a   = it->second;
        const uint32 now = calendarNow();
        if (a.trip.turbo)
        {
            // an hour's casts, some 100, over the debug venture's minute
            for (int i = 0; i < kTurboCasts; ++i)
            {
                if (const auto stop = turboCast(a, now); stop != Stop::None)
                {
                    return stop;
                }
            }
            return Stop::None;
        }
        if (now < a.at)
        {
            return Stop::None;
        }
        switch (a.step)
        {
            case Step::Ready:
                return cast(a, now);
            case Step::Biting:
                bite(a, now);
                break;
            case Step::Fighting:
                land(a, now);
                break;
        }
        return Stop::None;
    }

    void stop(const uint32 charid, const Worn& worn)
    {
        auto&                        list = anglers();
        const auto                   it   = list.find(charid);
        std::unique_ptr<CCharEntity> loaded;
        CCharEntity*                 PBody = nullptr;
        if (it != list.end())
        {
            PBody = it->second.body.get();
        }
        else
        {
            loaded = loadBody(charid);
            PBody  = loaded.get();
            if (PBody != nullptr)
            {
                pawn::bodies::hold(charid, PBody);
            }
        }
        if (PBody != nullptr)
        {
            if (PBody->hookedFish != nullptr || PBody->animation != xi::Animation::None)
            {
                fishingutils::InterruptFishing(PBody);
            }
            if (worn.known)
            {
                pawn::items::unequip(PBody, SLOT_AMMO);
                pawn::items::unequip(PBody, SLOT_RANGED);
                if (worn.ranged != 0)
                {
                    wear(PBody, worn.ranged, SLOT_RANGED, false);
                }
                if (worn.ammo != 0)
                {
                    wear(PBody, worn.ammo, SLOT_AMMO, false);
                }
            }
            PBody->clearPacketList();
            persist::flush(PBody, IsLogout::Yes);
            ShowInfoFmt("angler: {} stops fishing, skill {}", nameOf(charid), skillText(PBody));
            pawn::bodies::letGo(charid);
        }
        // done, or still selling what she caught (venture_sales.h); and how
        // long the venture lasted, by the game clock, never past its length
        const uint32 now = earth_time::game_timestamp();
        db::preparedStmt("UPDATE cardian_venture_report SET phase = IF(EXISTS (SELECT 1 FROM cardian_venture_items WHERE charid = ? AND `where` IN (?, ?)), ?, ?), "
                         "minutes = LEAST(length, IF(? > left_at, (? - left_at) DIV 60, 0)), updated = ? WHERE charid = ?",
                         charid, static_cast<uint8>(CL_VENTURE_CARRIED), static_cast<uint8>(CL_VENTURE_LISTED), static_cast<uint8>(CL_VENTURE_SELLING),
                         static_cast<uint8>(CL_VENTURE_DONE), now, now, realNow(), charid);
        if (it != list.end())
        {
            list.erase(it);
        }
    }
} // namespace pawn::angler
