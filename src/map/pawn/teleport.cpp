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

#include "teleport.h"

#include "action_keys.h"
#include "cardian_link_messages.h"
#include "party_finder.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "item_moves.h"
#include "pawn_items.h"
#include "players.h"
#include "role_support.h"
#include "seats.h"
#include "world.h"

#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "besieged_system.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/xirand.h"
#include "data/enums/key_item.h"
#include "entities/char_entity.h"
#include "items.h"
#include "items/transactions/item_claim.h"
#include "enums/chat_message_type.h"
#include "packets/s2c/0x017_chat_std.h"
#include "party.h"
#include "spell.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::teleport
{
    static_assert(kCrags[0].keyItem == static_cast<uint16>(xi::KeyItem::HollaGateCrystal) &&
                  kCrags[1].keyItem == static_cast<uint16>(xi::KeyItem::DemGateCrystal) &&
                  kCrags[2].keyItem == static_cast<uint16>(xi::KeyItem::MeaGateCrystal) &&
                  kCrags[3].keyItem == static_cast<uint16>(xi::KeyItem::AltepaGateCrystal) &&
                  kCrags[4].keyItem == static_cast<uint16>(xi::KeyItem::YhoatorGateCrystal) &&
                  kCrags[5].keyItem == static_cast<uint16>(xi::KeyItem::VahzlGateCrystal));
    static_assert(kCrags[0].spell == static_cast<uint16>(SpellID::Teleport_Holla) &&
                  kCrags[1].spell == static_cast<uint16>(SpellID::Teleport_Dem) &&
                  kCrags[2].spell == static_cast<uint16>(SpellID::Teleport_Mea) &&
                  kCrags[3].spell == static_cast<uint16>(SpellID::Teleport_Altep) &&
                  kCrags[4].spell == static_cast<uint16>(SpellID::Teleport_Yhoat) &&
                  kCrags[5].spell == static_cast<uint16>(SpellID::Teleport_Vahzl));

    namespace
    {
        using namespace std::chrono_literals;

        constexpr auto kLookEvery  = 250ms;
        constexpr auto kOrderAgain = 5s; // her order of the spell would not start: the next try waits this long

        // One hire, from her joining his party to the members she left
        // behind setting out to meet him
        struct Hire
        {
            enum class Stage : uint8
            {
                Coming,    // on her way to him
                Paying,    // beside him, her price said: his trade of it awaited
                Gathering, // paid, the party gathering round her
                Casting,   // the spell ordered, until it lands or comes to nothing
                Landing,   // she went too: until she has arrived
                Leaving,   // her last line said, she leaves the party at leaveAt
                Meeting,   // she has gone: the members left behind set out to meet him
            };

            uint32            player = 0;
            uint32            mage   = 0;
            uint8             log    = 0;
            uint32            price  = 0;
            uint32            paid   = 0;     // the gil he has paid her: her price, or nothing yet
            bool              inTown = false; // hired while he stood in a city: she keeps to town (rules::keepsToTown)
            uint32            seed   = 0;     // her words for this hire
            Stage             stage  = Stage::Coming;
            timer::time_point since{};       // this stage began: a gathering's patience is counted from here
            timer::time_point resolvedAt{};  // the spell landed
            timer::time_point leaveAt{};
            timer::time_point nextOrderAt{}; // her last order of the spell would not start: none again before this
            int               tries      = 0;
            bool              cast       = false; // her cast was seen under way
            bool              saidPrice  = false;
            bool              saidGather = false;
            bool              landed     = false; // the spell landed (else she gave up)
            bool              heGoes     = false; // the spell took him too: she waits for him at the crag
            bool              ownCast    = false; // her own order of the crag's Teleport stands: the one order the lock lets through
            bool              parted     = false; // the game took her, or him, out of the party (noteLeftParty)
            realtime::time_point heHere{};        // he stood at the crag with her from then; {} while he does not (a stopwatch, beside his client's packets)
            std::vector<uint32> moved; // the cardians of his party beside her as it landed: each meets him once he is settled
        };

        std::unordered_map<uint32, Hire> hires;  // by the player's charid
        std::vector<uint32>              strays; // under a teleport contract with no hire: out of the party on the next look
        timer::time_point                lastLook{};

        auto secondsSince(const timer::time_point at) -> double
        {
            return std::chrono::duration<double>(timer::now() - at).count();
        }

        // One of her lines, by her charid: the same hire gets the same words
        auto pick(const uint32 seed, std::initializer_list<const char*> lines) -> std::string
        {
            return *(lines.begin() + seed % lines.size());
        }

        auto spellOf(const Crag& crag) -> CSpell*
        {
            return spell::GetSpell(static_cast<SpellID>(crag.spell));
        }

        // A party member as the spell sees her, from the caster
        auto measure(CCharEntity* PMember, const CCharEntity* PMage, const Crag& crag) -> rules::Member
        {
            rules::Member m;
            m.sameZone   = PMember->loc.zone != nullptr && PMember->loc.zone == PMage->loc.zone;
            m.alive      = !PMember->isDead();
            m.hasCrystal = charutils::hasKeyItem(PMember, static_cast<xi::KeyItem>(crag.keyItem));
            m.mounted    = PMember->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Mounted);
            m.distance   = distance(PMember->loc.p, PMage->loc.p);
            m.rise       = std::fabs(PMember->loc.p.y - PMage->loc.p.y);
            return m;
        }

        // The characters of his party, he first: the game's party list, or
        // him alone
        auto partyOf(CCharEntity* PPlayer) -> std::vector<CCharEntity*>
        {
            std::vector<CCharEntity*> out{ PPlayer };
            if (PPlayer->PParty == nullptr)
            {
                return out;
            }
            for (auto* PMember : PPlayer->PParty->members)
            {
                if (PMember != PPlayer && PMember->objtype == TYPE_PC)
                {
                    out.push_back(static_cast<CCharEntity*>(PMember));
                }
            }
            return out;
        }

        // On her way to the crag: the spell's effect on her, or her zone
        // change already asked for
        auto carried(const CCharEntity* PChar, const Crag& crag) -> bool
        {
            const auto* PEffect = PChar->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Teleport);
            return (PEffect != nullptr && PEffect->GetPower() == crag.teleport) ||
                   (PChar->requestedZoneChange && PChar->loc.destination == static_cast<xi::ZoneId>(crag.zone));
        }

        // Casting the crag's Teleport now
        auto castingIt(const CCharEntity* PMage, const Crag& crag) -> bool
        {
            const auto* PMagic = dynamic_cast<const CMagicState*>(PMage->PAI->GetCurrentState());
            return PMagic != nullptr && PMagic->GetSpell() != nullptr && static_cast<uint16>(PMagic->GetSpell()->getID()) == crag.spell;
        }

        // The order still in her line: waiting, or the one she carries out
        auto inHerLine(CPawnController* PController, const Crag& crag) -> bool
        {
            const auto line = PController->QueueLine();
            const auto ours = [&](const cl_action& a)
            {
                return a.kind == CL_AK_MAGIC && a.id == crag.spell;
            };
            if (ours(line.action) || ours(line.running.action))
            {
                return true;
            }
            for (uint8 i = 0; i < line.more && i < 3; ++i)
            {
                if (ours(line.next[i].action))
                {
                    return true;
                }
            }
            return false;
        }

        auto controllerOf(CCharEntity* PPawn) -> CPawnController*
        {
            return PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
        }

        void say(CCharEntity* PMage, const std::string& line)
        {
            ShowInfoFmt("teleport: {} says in party chat: {}", PMage->getName(), line);
            pawn::tactics::role::sayParty(PMage, line);
        }

        auto thanksOf(const CCharEntity* PMage) -> std::string
        {
            return pick(PMage->id, { "Safe travels!", "Glad I could help. Take care out there!", "Enjoy the view from the crag!" });
        }

        // Her last line said: out of the party a moment later, so the line
        // reaches him before the party may break up under it
        void leaveSoon(Hire& h)
        {
            h.stage   = Hire::Stage::Leaving;
            h.since   = timer::now();
            h.leaveAt = timer::now() + std::chrono::duration_cast<timer::duration>(std::chrono::duration<double>(rules::kLeaveAfter));
        }

        // Her hire over: out of his party (her contract ends with it,
        // party_finder.h noteLeft) -- she goes about her business
        void leaveParty(CCharEntity* PMage)
        {
            if (PMage->PParty != nullptr)
            {
                ShowInfoFmt("teleport: {} leaves the party", PMage->getName());
                PMage->PParty->RemoveMember(PMage);
            }
            pawn::forgetGuestGambits(PMage->id);
        }

        // Gil from her purse into his delivery box, as one player sends
        // another gil: one move (item_moves.h), so a write that fails leaves
        // her purse as it was
        auto mailGil(CCharEntity* PMage, const uint32 playerCharID, const uint32 gil) -> bool
        {
            using namespace pawn::itemmoves;
            Txn txn;
            if (!txn.ok())
            {
                return false;
            }
            auto items = ItemClaimTransaction::start(PMage);
            if (items == nullptr)
            {
                return false;
            }
            const auto purse = std::vector<pawn::itemmoves::Slot>{ { PMage, LOC_INVENTORY, 0 } };
            const auto what  = fmt::format("{}'s {} gil back to {}", PMage->getName(), gil, pawn::seats::nameOf(playerCharID));
            if (!items->pay(gil))
            {
                abandon(txn, *items, std::nullopt, purse);
                return false;
            }
            const auto mark = newMark();
            const auto sent = db::preparedStmt("INSERT INTO delivery_box (charid, box, itemid, quantity, senderid, sender) VALUES (?, 1, ?, ?, ?, ?)", playerCharID,
                                               static_cast<uint16>(ITEMID::GIL), gil, PMage->id, PMage->getName());
            if (!sent || sent->rowsAffected() == 0 || !writeMark(mark))
            {
                ShowWarningFmt("teleport: {} could not be written: put back", what);
                abandon(txn, *items, std::nullopt, purse);
                return false;
            }
            return commitMove(txn, *items, mark, std::nullopt, purse, what);
        }

        // What he paid, back by his delivery box, and she says so: in party
        // chat while she is in his party, else in a tell
        void giveBack(Hire& h, CCharEntity* PMage)
        {
            if (h.paid == 0)
            {
                return;
            }
            if (!mailGil(PMage, h.player, h.paid))
            {
                ShowWarningFmt("teleport: {}'s {} gil back to {} did not go", PMage->getName(), h.paid, pawn::seats::nameOf(h.player));
                return;
            }
            ShowInfoFmt("teleport: {} sends {} back his {} gil by the delivery box", PMage->getName(), pawn::seats::nameOf(h.player), h.paid);
            const auto line    = fmt::format("I've sent your {} gil back by delivery.", rules::gilWords(h.paid));
            auto*      PPlayer = zoneutils::GetChar(h.player);
            if (PPlayer != nullptr && PMage->PParty != nullptr && PMage->PParty == PPlayer->PParty)
            {
                say(PMage, line);
            }
            else if (PPlayer != nullptr)
            {
                PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PMage, MESSAGE_TELL, line);
            }
            h.paid = 0;
        }

        // She goes home after a hire, wherever it ended: Warp, which every
        // White Mage who answers holds (rules::answers), to her home point --
        // a teleporter's, her hub's (census.py TELEPORTER); already in its
        // city, she stays
        void warpHome(CCharEntity* PMage)
        {
            auto* PController = controllerOf(PMage);
            auto* PHome       = zoneutils::GetZone(PMage->profile.home_point.destination);
            if (PController == nullptr || PMage->loc.zone == nullptr || PHome == nullptr || PMage->loc.zone == PHome ||
                pawn::sameCity(PMage->loc.zone, PHome))
            {
                return;
            }
            const auto status = PController->DoAction(keyOfAction({ CL_AK_MAGIC, 2, static_cast<uint16>(SpellID::Warp) }), PMage);
            if (status == CL_S_OK)
            {
                ShowInfoFmt("teleport: {} casts Warp to go home to {}", PMage->getName(), PHome->getName());
            }
            else
            {
                ShowWarningFmt("teleport: {}'s Warp home to {} would not start (0x{:04X}): she fades where she is and goes home that way", PMage->getName(),
                               PHome->getName(), status);
            }
        }

        // She gives up: a line, then out of the party a moment later. What he
        // paid is hers (the user, 2026-10-21: paid is paid), unless she
        // cannot get the spell off (cannotCast)
        void giveUp(Hire& h, CCharEntity* PMage, const std::string& line, const std::string& why)
        {
            ShowInfoFmt("teleport: {} gives up on {}'s teleport: {}", PMage->getName(), pawn::seats::nameOf(h.player), why);
            say(PMage, line);
            h.landed = false;
            leaveSoon(h);
        }

        // Her own failure, the one refund: three tries at the spell that came
        // to nothing
        void cannotCast(Hire& h, CCharEntity* PMage, const std::string& why)
        {
            giveUp(h, PMage, "Sorry, I can't get the spell off.", why);
            giveBack(h, PMage);
        }

        // The spell has landed: who stays behind and why, in party chat, and
        // the cardians beside her noted, to meet him once he is settled. Read
        // off her party, which holds everyone beside her even while he is
        // between zones (PPlayer nullptr then)
        void resolve(Hire& h, CCharEntity* PPlayer, CCharEntity* PMage, const Crag& crag)
        {
            h.ownCast    = false;
            h.landed     = true;
            pawn::finder::endShout(h.player); // the shout that hired her has done its work
            h.resolvedAt = timer::now();
            for (auto* PMember : partyOf(PMage))
            {
                if (PMember == PMage || PMember->loc.zone != PMage->loc.zone)
                {
                    continue;
                }
                if (pawn::isPawn(PMember))
                {
                    h.moved.push_back(PMember->id);
                }
                if (carried(PMember, crag))
                {
                    h.heGoes = h.heGoes || PMember == PPlayer;
                    ShowInfoFmt("teleport: {} goes to {}", PMember->getName(), crag.name);
                    continue;
                }
                auto stay = rules::stayOf(measure(PMember, PMage, crag));
                if (stay == rules::Stay::Goes)
                {
                    stay = rules::Stay::OutOfRange; // in reach now, not as it landed
                }
                say(PMage, fmt::format("{} stays behind. {}", PMember->getName(), rules::reasonOf(stay, crag.name)));
            }

            if (carried(PMage, crag))
            {
                ShowInfoFmt("teleport: {} goes to {} too, and leaves the party once there", PMage->getName(), crag.name);
                h.stage = Hire::Stage::Landing;
                h.since = timer::now();
                return;
            }
            say(PMage, thanksOf(PMage));
            leaveSoon(h);
        }

        // The cardians beside her as it landed, gone with him or left
        // behind, each set out to meet him wherever he is now: the trek's
        // own travel order (pawn.h orderTravel), the automatic hold his
        // teleport set lifted first. A hold he ordered stays his
        void sendToMeet(const Hire& h, CCharEntity* PPlayer)
        {
            for (const uint32 charid : h.moved)
            {
                auto* PPawn = pawn::findPawn(charid);
                if (PPawn == nullptr || PPawn->PParty == nullptr || PPawn->PParty != PPlayer->PParty || PPawn->loc.zone == nullptr ||
                    PPawn->getZone() == PPlayer->getZone())
                {
                    continue;
                }
                auto* PController = controllerOf(PPawn);
                if (PController != nullptr && PController->IsWaiting() && !PController->HoldsOnOrder())
                {
                    PController->SetWaiting(false, false, fmt::format("sets out to meet {}", PPlayer->getName()));
                }
                if (pawn::orderTravel(charid, static_cast<uint16>(PPlayer->getZone()), PPlayer->id))
                {
                    ShowInfoFmt("teleport: {} sets out to meet {} in {}", PPawn->getName(), PPlayer->getName(), PPlayer->loc.zone->getName());
                }
            }
        }

        // He stands in a zone, not on his way to another
        auto settled(const CCharEntity* PPlayer) -> bool
        {
            if (PPlayer == nullptr || PPlayer->loc.zone == nullptr || PPlayer->requestedZoneChange)
            {
                return false;
            }
            const auto headed = PPlayer->loc.destination;
            return headed == xi::ZoneId::Unknown || headed == ZONE_NO_DESTINATION || headed == PPlayer->getZone();
        }

        // Paid, the party gathering round her: she orders the spell once
        // everyone of her party who could go stands gathered, or the
        // patience since the gathering began has run out, wherever anyone
        // stands -- he included (the user, 2026-10-21: paid is paid)
        void gather(Hire& h, CCharEntity* PMage, CPawnController* PController, const Crag& crag)
        {
            // Nothing of her own under way, and a moment after an order that would not start
            if (PMage->isDead() || !PMage->PAI->CanChangeState() || timer::now() < h.nextOrderAt)
            {
                return;
            }
            std::vector<rules::Member> party;
            bool                       him = false;
            for (auto* PMember : partyOf(PMage))
            {
                if (PMember != PMage)
                {
                    party.push_back(measure(PMember, PMage, crag));
                    him = him || PMember->id == h.player;
                }
            }
            if (!him)
            {
                party.push_back(rules::Member{ .sameZone = false }); // he is between zones: away, for the patience
            }
            const double waited = secondsSince(h.since);
            if (!rules::ready(party, waited))
            {
                return;
            }
            h.ownCast         = true;
            const auto status = PController->DoAction(keyOfAction({ CL_AK_MAGIC, 2, crag.spell }), PMage);
            if (status != CL_S_OK)
            {
                h.ownCast     = false;
                h.nextOrderAt = timer::now() + kOrderAgain;
                ++h.tries;
                ShowInfoFmt("teleport: {}'s order of Teleport-{} would not start (0x{:04X}), try {} of {}", PMage->getName(), crag.name, status, h.tries,
                            rules::kTriesAtMost);
                if (h.tries >= rules::kTriesAtMost)
                {
                    cannotCast(h, PMage, "the order never started");
                }
                return;
            }
            ShowInfoFmt("teleport: {} casts Teleport-{} for {}'s party ({} s gathered)", PMage->getName(), crag.name, pawn::seats::nameOf(h.player),
                        static_cast<int>(waited));
            h.stage = Hire::Stage::Casting;
            h.since = timer::now();
            h.cast  = false;
        }

        // One hire's step; false once it is over
        auto step(Hire& h) -> bool
        {
            const auto* crag    = cragOf(h.log);
            auto*       PMage   = pawn::findPawn(h.mage);
            auto*       PPlayer = zoneutils::GetChar(h.player); // nullptr while he is between zones
            if (crag == nullptr || PMage == nullptr)
            {
                ShowInfoFmt("teleport: {}'s hire of {} ends: she is gone from the world", pawn::seats::nameOf(h.player), pawn::seats::nameOf(h.mage));
                return false;
            }

            // Out of the world, he is out of the hire
            if (!pawn::players::online(h.player))
            {
                ShowInfoFmt("teleport: {} has left the world: {}'s hire ends", pawn::seats::nameOf(h.player), PMage->getName());
                leaveParty(PMage);
                return false;
            }

            if (h.stage == Hire::Stage::Meeting)
            {
                if (secondsSince(h.resolvedAt) < rules::kSettleAfter || !settled(PPlayer))
                {
                    return true;
                }
                sendToMeet(h, PPlayer);
                return false;
            }

            if (h.stage == Hire::Stage::Leaving)
            {
                if (timer::now() < h.leaveAt)
                {
                    return true;
                }
                leaveParty(PMage);
                warpHome(PMage);
                if (!h.landed)
                {
                    return false;
                }
                h.stage = Hire::Stage::Meeting;
                h.since = timer::now();
                return true;
            }

            // Out of one party -- he let her go, he left it, or it broke up:
            // the game's own word (noteLeftParty), which a zone change never
            // gives; or she is in no party at all, which the game can also
            // leave her without a word (charutils::ReloadParty), and her own
            // party survives her zone changes. Her cast under way stops, so
            // she never lands at the crag alone. Landed, she still goes home,
            // and the members left behind are still sent to meet him
            if (h.parted || PMage->PParty == nullptr)
            {
                ShowInfoFmt("teleport: {} and {} share a party no longer: the hire ends", PMage->getName(), pawn::seats::nameOf(h.player));
                if (castingIt(PMage, *crag))
                {
                    PMage->PAI->InterruptStates();
                }
                leaveParty(PMage);
                if (!h.landed)
                {
                    return false;
                }
                warpHome(PMage);
                h.stage = Hire::Stage::Meeting;
                h.since = timer::now();
                return true;
            }

            // Hired in town and not yet casting, she keeps to town: he has
            // left it, and she leaves his party
            const bool castUnderWay = h.stage != Hire::Stage::Coming && h.stage != Hire::Stage::Paying && h.stage != Hire::Stage::Gathering;
            if (settled(PPlayer) && !rules::keepsToTown(h.inTown, castUnderWay, isTown(PPlayer->loc.zone)))
            {
                ShowInfoFmt("teleport: {} has left town for {}: {} goes about her business", PPlayer->getName(), PPlayer->loc.zone->getName(), PMage->getName());
                say(PMage, pick(h.seed, { "Not going after all? I'll be off, then.", "Changed your mind? No worries. Take care!", "Guess you won't need me. Safe travels!" }));
                h.landed = false;
                leaveSoon(h);
                return true;
            }

            auto* PController = controllerOf(PMage);
            if (PController == nullptr)
            {
                return false;
            }

            switch (h.stage)
            {
                case Hire::Stage::Coming:
                {
                    // No limit on her way to him: he is in charge, and dismisses her if she never comes
                    if (PPlayer == nullptr || PPlayer->loc.zone == nullptr || PPlayer->loc.zone != PMage->loc.zone || PMage->isDead() ||
                        distance(PPlayer->loc.p, PMage->loc.p) > rules::kBesideYalms)
                    {
                        return true;
                    }
                    ShowInfoFmt("teleport: {} has reached {} in {}", PMage->getName(), PPlayer->getName(), PPlayer->loc.zone->getName());
                    h.since = timer::now();
                    if (h.paid < h.price)
                    {
                        h.stage = Hire::Stage::Paying;
                        if (!h.saidPrice)
                        {
                            h.saidPrice = true;
                            say(PMage, rules::priceLine(h.seed, h.price));
                        }
                        return true;
                    }
                    h.stage = Hire::Stage::Gathering;
                    if (!h.saidGather)
                    {
                        h.saidGather = true;
                        say(PMage, fmt::format("Gather round for Teleport-{}.", crag->name));
                    }
                    return true;
                }
                case Hire::Stage::Paying:
                {
                    // She waits for his trade as long as he likes (paid)
                    if (settled(PPlayer) && PPlayer->loc.zone != PMage->loc.zone)
                    {
                        ShowInfoFmt("teleport: {} is no longer beside {}, and comes to him again", PMage->getName(), PPlayer->getName());
                        h.stage = Hire::Stage::Coming;
                        h.since = timer::now();
                    }
                    return true;
                }
                case Hire::Stage::Gathering:
                    gather(h, PMage, PController, *crag);
                    return true;
                case Hire::Stage::Casting:
                {
                    if (castingIt(PMage, *crag))
                    {
                        h.cast = true;
                        return true;
                    }
                    // It has landed when anyone of her party beside her is on the
                    // way, she included -- he may be between zones meanwhile
                    for (auto* PMember : partyOf(PMage))
                    {
                        if (PMember->loc.zone == PMage->loc.zone && carried(PMember, *crag))
                        {
                            resolve(h, PPlayer, PMage, *crag);
                            return true;
                        }
                    }
                    // Still waiting in her line to start, for a while
                    const bool waiting = !h.cast && inHerLine(PController, *crag) && secondsSince(h.since) < rules::kStartWithin;
                    if (waiting || (!h.cast && secondsSince(h.since) < 1.0))
                    {
                        return true;
                    }
                    ++h.tries;
                    h.ownCast = false;
                    ShowInfoFmt("teleport: {}'s Teleport-{} came to nothing ({}), try {} of {}", PMage->getName(), crag->name,
                                h.cast ? "interrupted" : "never started", h.tries, rules::kTriesAtMost);
                    if (!h.cast && inHerLine(PController, *crag))
                    {
                        PController->ClearQueuedOrders("her teleport never started");
                    }
                    if (h.tries >= rules::kTriesAtMost)
                    {
                        cannotCast(h, PMage, "every cast came to nothing");
                        return true;
                    }
                    h.stage = Hire::Stage::Gathering;
                    h.since = timer::now();
                    return true;
                }
                case Hire::Stage::Landing:
                {
                    // At the crag: her thanks, then out of the party -- once he
                    // stands there with her and his client is in, when the spell
                    // took him too: a line said while he loads the zone never
                    // reaches him, and with nobody in her zone the world fades her
                    // before her Warp home is off
                    const bool there  = PMage->loc.zone != nullptr && static_cast<uint16>(PMage->getZone()) == crag->zone && !PMage->requestedZoneChange;
                    const bool heHere = PPlayer != nullptr && settled(PPlayer) && PPlayer->loc.zone == PMage->loc.zone;
                    if (!heHere)
                    {
                        h.heHere = {};
                    }
                    else if (h.heHere == realtime::time_point{})
                    {
                        h.heHere = realtime::now();
                    }
                    const auto since   = std::chrono::duration_cast<std::chrono::milliseconds>(realtime::now() - h.heHere);
                    const auto age     = pawn::positionPacketAge(h.player);
                    const bool hisIsIn = heHere && since >= std::chrono::seconds(2) && age.has_value() && *age < since;
                    if ((!there || (h.heGoes && !hisIsIn)) && secondsSince(h.since) < 60.0)
                    {
                        return true;
                    }
                    say(PMage, thanksOf(PMage));
                    leaveSoon(h);
                    return true;
                }
                default:
                    return true;
            }
        }
    } // namespace

    auto shoutRefusal(const CCharEntity* PPlayer, const uint8 log) -> uint16
    {
        const auto* crag = cragOf(log);
        if (PPlayer == nullptr || crag == nullptr)
        {
            return CL_S_MALFORMED;
        }
        if (!isTown(PPlayer->loc.zone))
        {
            return CL_S_TELEPORT_IN_TOWN;
        }
        if (!charutils::hasKeyItem(PPlayer, static_cast<xi::KeyItem>(crag->keyItem)))
        {
            return CL_S_NO_GATE_CRYSTAL;
        }
        if (hires.contains(PPlayer->id))
        {
            return CL_S_TELEPORT_HIRED;
        }
        return CL_S_OK;
    }

    auto hiringFor(const uint32 playerCharID) -> bool
    {
        return hires.contains(playerCharID);
    }

    auto cragsHeld(const CCharEntity* PPlayer) -> std::vector<const Crag*>
    {
        std::vector<const Crag*> out;
        for (const auto& crag : kCrags)
        {
            if (PPlayer != nullptr && charutils::hasKeyItem(PPlayer, static_cast<xi::KeyItem>(crag.keyItem)))
            {
                out.push_back(&crag);
            }
        }
        return out;
    }

    Responders::Responders(const uint8 log)
    : crag_(cragOf(log))
    {
        if (crag_ == nullptr)
        {
            return;
        }
        const auto rset = db::preparedStmt("SELECT charid FROM char_spells WHERE spellid = ?", crag_->spell);
        while (rset && rset->next())
        {
            knowers_.insert(rset->get<uint32>("charid"));
        }
        const auto warp = db::preparedStmt("SELECT charid FROM char_spells WHERE spellid = ?", static_cast<uint16>(SpellID::Warp));
        while (warp && warp->next())
        {
            warpers_.insert(warp->get<uint32>("charid"));
        }
    }

    auto Responders::answers(const uint32 charid, const CCharEntity* PPawn, const uint8 mjob, const uint8 mlvl, const uint8 sjob, const uint8 slvl) const -> bool
    {
        auto* PSpell = crag_ != nullptr ? spellOf(*crag_) : nullptr;
        auto* PWarp  = spell::GetSpell(SpellID::Warp);
        if (PSpell == nullptr || PWarp == nullptr)
        {
            return false;
        }
        if (PPawn != nullptr)
        {
            auto* PChar = const_cast<CCharEntity*>(PPawn);
            return rules::answers({ charutils::hasSpell(PChar, crag_->spell) != 0, spell::CanUseSpell(PChar, PSpell) }, static_cast<uint32>(std::max(0, PPawn->health.mp)),
                                  PSpell->getMPCost(), { charutils::hasSpell(PChar, static_cast<uint16>(SpellID::Warp)) != 0, spell::CanUseSpell(PChar, PWarp) });
        }
        // Faded, she stands whole for the invite (world.cpp): her MP is her full MP
        const auto castsAt = [&](CSpell* PCast)
        {
            return rules::castsAt(PCast->getJob(static_cast<xi::Job>(mjob)), mlvl, PCast->getJob(static_cast<xi::Job>(sjob)), slvl);
        };
        return rules::answers({ knowers_.contains(charid), castsAt(PSpell) }, std::numeric_limits<uint32>::max(), PSpell->getMPCost(),
                              { warpers_.contains(charid), castsAt(PWarp) });
    }

    auto answerLine(const uint32 seed, const uint8 log) -> std::string
    {
        const auto* crag = cragOf(log);
        return rules::quoteLine(seed, crag != nullptr ? crag->name : std::string_view("the crag"));
    }

    void hired(const uint32 playerCharID, const uint32 mageCharID, const uint8 log, const bool fresh, const uint32 price)
    {
        auto* PMage = pawn::findPawn(mageCharID);
        if (PMage == nullptr)
        {
            return;
        }
        // A hire starts on a yes to his shout alone: a teleport contract
        // never outlives his session nor the map (signOut,
        // endContractsAtBoot), so an invite of an open one is a stray, out of
        // the party without a word
        if (!fresh)
        {
            ShowInfoFmt("teleport: {} joins {} under a teleport contract no shout of his made: she leaves the party", PMage->getName(),
                        pawn::seats::nameOf(playerCharID));
            strays.push_back(mageCharID);
            return;
        }
        if (cragOf(log) == nullptr)
        {
            return;
        }
        // One White Mage at a time: the finder sends no second teleport
        // invite while one stands (party_finder.h teleportPending), so a
        // second yes is a stray too
        if (const auto it = hires.find(playerCharID); it != hires.end() && it->second.mage != mageCharID)
        {
            ShowWarningFmt("teleport: {} joins {} while {} is hired already: she leaves the party", PMage->getName(), pawn::seats::nameOf(playerCharID),
                           pawn::seats::nameOf(it->second.mage));
            strays.push_back(mageCharID);
            return;
        }
        // Her MP full for every hire: she never waits for it
        PMage->addMP(PMage->GetMaxMP());
        const auto* PPlayer = zoneutils::GetChar(playerCharID);
        Hire        hire{ .player = playerCharID,
                          .mage   = mageCharID,
                          .log    = log,
                          .price  = price,
                          .inTown = PPlayer != nullptr && isTown(PPlayer->loc.zone),
                          .seed   = xirand::GetRandomNumber(1000u),
                          .since  = timer::now() };
        ShowInfoFmt("teleport: {} is hired by {} for Teleport-{} at {} gil{}", PMage->getName(), pawn::seats::nameOf(playerCharID), cragOf(log)->name,
                    price, hire.inTown ? ", in town" : "");
        hires.insert_or_assign(playerCharID, std::move(hire));
    }

    void signOut(const CCharEntity* PPlayer)
    {
        if (PPlayer == nullptr)
        {
            return;
        }
        const auto ends = [&](Hire& h)
        {
            if (h.player != PPlayer->id)
            {
                return false;
            }
            if (auto* PMage = pawn::findPawn(h.mage); PMage != nullptr)
            {
                ShowInfoFmt("teleport: {} signs out: {}'s hire ends", PPlayer->getName(), PMage->getName());
                leaveParty(PMage);
            }
            return true;
        };
        std::erase_if(hires, [&](auto& entry) { return ends(entry.second); });
    }

    void noteLeftParty(const uint32 charid)
    {
        for (auto& [player, h] : hires)
        {
            if (h.mage == charid || h.player == charid)
            {
                h.parted = true;
            }
        }
    }

    void joinFailed(const uint32 mageCharID)
    {
        std::erase_if(hires, [&](const auto& entry)
                      {
                          if (entry.second.mage != mageCharID)
                          {
                              return false;
                          }
                          ShowInfoFmt("teleport: {} never got into {}'s party: the hire ends", pawn::seats::nameOf(mageCharID), pawn::seats::nameOf(entry.first));
                          return true;
                      });
    }

    auto isTown(CZone* PZone) -> bool
    {
        return PZone != nullptr && ((PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown || PZone->GetID() == xi::ZoneId::Nashmau);
    }

    namespace
    {
        // The beastmen march on Al Zahbi (Besieged): any of their three forces
        // ordered to attack
        auto besiegedUnderWay() -> bool
        {
            return besieged::GetMamookOrders() == ORDERS_ATTACK || besieged::GetHalvungOrders() == ORDERS_ATTACK ||
                   besieged::GetArrapagoOrders() == ORDERS_ATTACK;
        }
    } // namespace

    auto hubOf(CZone* PZone) -> uint32
    {
        if (PZone == nullptr)
        {
            return 0;
        }
        if (PZone->GetID() == xi::ZoneId::AlZahbi && !besiegedUnderWay())
        {
            return static_cast<uint32>(xi::ZoneId::AhtUrhganWhitegate);
        }
        const auto region = PZone->GetRegionID();
        if (isTown(PZone) && region >= REGION_TYPE::SANDORIA && region <= REGION_TYPE::JEUNO)
        {
            return 0x10000u | static_cast<uint32>(region);
        }
        return static_cast<uint32>(PZone->GetID());
    }

    void endContractsAtBoot()
    {
        const auto rset = db::preparedStmt("SELECT player_charid, pawn_charid FROM cardian_party_memory WHERE contract = 'teleport'");
        while (rset && rset->next())
        {
            ShowInfoFmt("teleport: at boot, {}'s teleport contract with {} ends", pawn::seats::nameOf(rset->get<uint32>("pawn_charid")),
                        pawn::seats::nameOf(rset->get<uint32>("player_charid")));
        }
        db::preparedStmt("UPDATE cardian_party_memory SET contract = '' WHERE contract = 'teleport'");
    }

    void giveCrystals(CCharEntity* PChar)
    {
        if (PChar == nullptr)
        {
            return;
        }
        bool given = false;
        for (const auto& crag : kCrags)
        {
            if (const auto crystal = static_cast<xi::KeyItem>(crag.keyItem); !charutils::hasKeyItem(PChar, crystal))
            {
                charutils::addKeyItem(PChar, crystal);
                given = true;
            }
        }
        if (given)
        {
            charutils::SaveKeyItems(PChar);
        }
    }

    auto locked(const CCharEntity* PMage) -> bool
    {
        return PMage != nullptr && PMage->PParty != nullptr &&
               std::ranges::any_of(hires, [&](const auto& entry) { return entry.second.mage == PMage->id && entry.second.stage != Hire::Stage::Meeting; });
    }

    auto ownsCast(const CCharEntity* PMage, const uint16 spellId) -> bool
    {
        return PMage != nullptr && std::ranges::any_of(hires, [&](const auto& entry)
                                                       {
                                                           const auto* crag = cragOf(entry.second.log);
                                                           return entry.second.mage == PMage->id && entry.second.ownCast && crag != nullptr && crag->spell == spellId;
                                                       });
    }

    auto isHired(const uint32 mageCharID) -> bool
    {
        return std::ranges::any_of(hires, [&](const auto& entry) { return entry.second.mage == mageCharID; });
    }

    auto priceAwaited(const uint32 mageCharID, const uint32 playerCharID) -> uint32
    {
        const auto it = hires.find(playerCharID);
        if (it == hires.end() || it->second.mage != mageCharID)
        {
            return 0;
        }
        const auto& h      = it->second;
        const bool  before = h.stage == Hire::Stage::Coming || h.stage == Hire::Stage::Paying;
        return before && h.paid < h.price ? h.price : 0;
    }

    void paid(const uint32 mageCharID)
    {
        for (auto& [player, h] : hires)
        {
            if (h.mage != mageCharID)
            {
                continue;
            }
            auto* PMage = pawn::findPawn(mageCharID);
            if (PMage == nullptr)
            {
                return;
            }
            h.paid = h.price;
            ShowInfoFmt("teleport: {} pays {} her {} gil", pawn::seats::nameOf(player), PMage->getName(), h.paid);
            if (h.stage == Hire::Stage::Paying)
            {
                const auto* crag = cragOf(h.log);
                h.stage          = Hire::Stage::Gathering;
                h.since          = timer::now();
                h.saidGather     = true;
                say(PMage, fmt::format("Thanks! Gather round for Teleport-{}.", crag != nullptr ? crag->name : std::string_view("the crag")));
            }
            return;
        }
    }

    void tick()
    {
        const auto now = timer::now();
        if ((hires.empty() && strays.empty()) || now - lastLook < kLookEvery)
        {
            return;
        }
        lastLook = now;
        for (const auto charid : std::exchange(strays, {}))
        {
            if (auto* PMage = pawn::findPawn(charid); PMage != nullptr && PMage->PParty != nullptr)
            {
                leaveParty(PMage);
            }
        }
        for (auto it = hires.begin(); it != hires.end();)
        {
            it = step(it->second) ? std::next(it) : hires.erase(it);
        }
    }
} // namespace pawn::teleport
