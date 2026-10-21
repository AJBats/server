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
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_items.h"
#include "players.h"
#include "role_support.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/xirand.h"
#include "data/enums/key_item.h"
#include "entities/char_entity.h"
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

        constexpr auto kLookEvery = 250ms;

        // Her crag, while she is hired: its log plus one, so a restart finds
        // it again when his invite of her open contract brings her back; her
        // price, and the gil he has paid her
        constexpr auto kCragVar  = "[CARDIAN]TeleportCrag";
        constexpr auto kPriceVar = "[CARDIAN]TeleportPrice";
        constexpr auto kPaidVar  = "[CARDIAN]TeleportPaid";

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
            timer::time_point hiredAt{};
            timer::time_point since{};         // this stage began
            timer::time_point beside{};        // she reached him: her wait is counted from here
            timer::time_point gatheredSince{}; // he has stood gathered since; {} while he is not
            timer::time_point resolvedAt{};    // the spell landed
            timer::time_point leaveAt{};
            int               tries      = 0;
            bool              joined     = false; // seen in his party: her yes is answered before the party takes her in
            bool              spare      = false; // a second White Mage of his: she bows out once in his party
            bool              cast       = false; // her cast was seen under way
            bool              saidPrice  = false;
            bool              saidGather = false;
            bool              saidMount  = false;
            bool              saidMp     = false;
            bool              landed     = false; // the spell landed (else she gave up)
            std::vector<uint32> moved; // the cardians of his party beside her as it landed: each meets him once he is settled
        };

        std::unordered_map<uint32, Hire> hires;  // by the player's charid
        std::vector<Hire>                spares; // a second yes he took while the first was answering
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

        // Her hire over: her crag forgotten, and out of his party (her
        // contract ends with it, party_finder.h noteLeft) -- she goes about
        // her business
        void forget(CCharEntity* PMage)
        {
            PMage->setCharVar(kCragVar, 0);
            PMage->setCharVar(kPriceVar, 0);
            PMage->setCharVar(kPaidVar, 0);
        }

        void leaveParty(CCharEntity* PMage)
        {
            forget(PMage);
            if (PMage->PParty != nullptr)
            {
                ShowInfoFmt("teleport: {} leaves the party", PMage->getName());
                PMage->PParty->RemoveMember(PMage);
            }
            pawn::forgetGuestGambits(PMage->id);
        }

        // A hire that ends without the spell landing gives back what he
        // paid: in party chat while she is in his party, else in a tell
        void giveBack(Hire& h, CCharEntity* PMage)
        {
            if (h.paid == 0)
            {
                return;
            }
            auto* PPlayer = zoneutils::GetChar(h.player);
            if (PPlayer == nullptr)
            {
                ShowWarningFmt("teleport: {} cannot give back {}'s {} gil: he is not in the world", PMage->getName(), pawn::seats::nameOf(h.player), h.paid);
                return;
            }
            if (const auto status = pawn::items::handGil(PMage, PPlayer, h.paid); status != CL_S_OK)
            {
                ShowWarningFmt("teleport: {}'s {} gil back to {} did not go (0x{:04X})", PMage->getName(), h.paid, PPlayer->getName(), status);
                return;
            }
            ShowInfoFmt("teleport: {} gives {} back his {} gil", PMage->getName(), PPlayer->getName(), h.paid);
            const auto line = fmt::format("Here's your {} gil back.", rules::gilWords(h.paid));
            if (PMage->PParty != nullptr && PMage->PParty == PPlayer->PParty)
            {
                say(PMage, line);
            }
            else
            {
                PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PMage, MESSAGE_TELL, line);
            }
            h.paid = 0;
            PMage->setCharVar(kPaidVar, 0);
        }

        // She gives up: a line, what he paid given back, then out of the
        // party a moment later
        void giveUp(Hire& h, CCharEntity* PMage, const std::string& line, const std::string& why)
        {
            ShowInfoFmt("teleport: {} gives up on {}'s teleport: {}", PMage->getName(), pawn::seats::nameOf(h.player), why);
            say(PMage, line);
            giveBack(h, PMage);
            h.landed = false;
            leaveSoon(h);
        }

        auto isCity(CZone* PZone) -> bool
        {
            return PZone != nullptr && (PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
        }

        // The spell has landed: who stays behind and why, in party chat, and
        // the cardians beside her noted, to meet him once he is settled
        void resolve(Hire& h, CCharEntity* PPlayer, CCharEntity* PMage, const Crag& crag)
        {
            h.landed     = true;
            h.resolvedAt = timer::now();
            for (auto* PMember : partyOf(PPlayer))
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

        // Beside him and gathering: ready, she orders the spell
        void gather(Hire& h, CCharEntity* PPlayer, CCharEntity* PMage, CPawnController* PController, const Crag& crag)
        {
            if (secondsSince(h.beside) > rules::kWaitAtMost)
            {
                giveUp(h, PMage, "Sorry, I can't wait any longer.", "the party never gathered");
                return;
            }
            if (PPlayer->loc.zone != PMage->loc.zone)
            {
                ShowInfoFmt("teleport: {} is no longer beside {}, and comes to him again", PMage->getName(), PPlayer->getName());
                h.stage = Hire::Stage::Coming;
                h.since = timer::now();
                return;
            }
            // Not in a fight, nor in the middle of anything of her own
            if (PPlayer->PAI->IsEngaged() || PMage->PAI->IsEngaged() || PMage->isDead() || !PMage->PAI->CanChangeState())
            {
                h.gatheredSince = {};
                return;
            }

            std::vector<rules::Member> others;
            for (auto* PMember : partyOf(PPlayer))
            {
                if (PMember != PPlayer && PMember != PMage)
                {
                    others.push_back(measure(PMember, PMage, crag));
                }
            }
            const auto player = measure(PPlayer, PMage, crag);
            if (!rules::gathered(player))
            {
                h.gatheredSince = {};
            }
            else if (h.gatheredSince == timer::time_point{})
            {
                h.gatheredSince = timer::now();
            }
            const double waited = h.gatheredSince == timer::time_point{} ? 0.0 : secondsSince(h.gatheredSince);
            const auto   ready  = rules::ready(player, others, waited);
            if (ready == rules::Ready::PlayerMounted && !h.saidMount)
            {
                h.saidMount = true;
                say(PMage, "Off the chocobo first, and we'll go.");
            }
            if (ready != rules::Ready::Cast)
            {
                return;
            }

            const auto* PSpell = spellOf(crag);
            if (PSpell == nullptr || PMage->health.mp < static_cast<int32>(PSpell->getMPCost()))
            {
                if (!h.saidMp)
                {
                    h.saidMp = true;
                    say(PMage, "Let me rest for my MP first.");
                }
                return;
            }
            const auto status = PController->DoAction(keyOfAction({ CL_AK_MAGIC, 2, crag.spell }), PMage);
            if (status != CL_S_OK)
            {
                ++h.tries;
                ShowInfoFmt("teleport: {}'s order of Teleport-{} would not start (0x{:04X}), try {} of {}", PMage->getName(), crag.name, status, h.tries,
                            rules::kTriesAtMost);
                if (h.tries >= rules::kTriesAtMost)
                {
                    giveUp(h, PMage, "Sorry, I can't get the spell off.", "the order never started");
                }
                return;
            }
            ShowInfoFmt("teleport: {} casts Teleport-{} for {}'s party ({} s gathered)", PMage->getName(), crag.name, PPlayer->getName(), static_cast<int>(waited));
            h.stage = Hire::Stage::Casting;
            h.since = timer::now();
            h.cast  = false;
        }

        // One hire's step; false once it is over
        auto step(Hire& h) -> bool
        {
            const auto* crag   = cragOf(h.log);
            auto*       PMage  = pawn::findPawn(h.mage);
            auto*       PPlayer = zoneutils::GetChar(h.player);
            if (crag == nullptr || PMage == nullptr)
            {
                ShowInfoFmt("teleport: {}'s hire of {} ends: she is gone from the world", pawn::seats::nameOf(h.player), pawn::seats::nameOf(h.mage));
                return false;
            }

            // Out of the world, he is out of the hire
            if (!pawn::players::online(h.player))
            {
                ShowInfoFmt("teleport: {} has left the world: {}'s hire ends", pawn::seats::nameOf(h.player), PMage->getName());
                giveBack(h, PMage);
                leaveParty(PMage);
                return false;
            }

            if (h.stage == Hire::Stage::Meeting)
            {
                if (secondsSince(h.resolvedAt) > 120.0)
                {
                    ShowInfoFmt("teleport: {} never settled after the teleport; nobody is sent to meet him", pawn::seats::nameOf(h.player));
                    return false;
                }
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
                if (!h.landed)
                {
                    return false;
                }
                h.stage = Hire::Stage::Meeting;
                h.since = timer::now();
                return true;
            }

            // Out of his party -- he let her go, or she was taken out -- the
            // hire is over
            const bool inParty = PPlayer != nullptr && PMage->PParty != nullptr && PMage->PParty == PPlayer->PParty;
            h.joined           = h.joined || inParty;
            if (!h.joined && secondsSince(h.hiredAt) < 30.0)
            {
                return true; // her yes answered, the party not yet taken her in
            }
            if (h.spare && inParty)
            {
                giveUp(h, PMage, "Looks like you're covered already. Safe travels!", "another White Mage was hired first");
                return true;
            }
            if (h.stage != Hire::Stage::Landing && !inParty)
            {
                if (PPlayer != nullptr && PPlayer->loc.zone != nullptr)
                {
                    ShowInfoFmt("teleport: {} is out of {}'s party: the hire ends", PMage->getName(), PPlayer->getName());
                    giveBack(h, PMage);
                    forget(PMage);
                    return false;
                }
                return true; // he is between zones: his party list is empty for a moment
            }

            auto* PController = controllerOf(PMage);
            if (PController == nullptr)
            {
                return false;
            }

            // Hired in town and not yet casting, she keeps to town: he has
            // left it, and she leaves his party
            const bool castUnderWay = h.stage != Hire::Stage::Coming && h.stage != Hire::Stage::Paying && h.stage != Hire::Stage::Gathering;
            if (settled(PPlayer) && !rules::keepsToTown(h.inTown, castUnderWay, isCity(PPlayer->loc.zone)))
            {
                ShowInfoFmt("teleport: {} has left town for {}: {} goes about her business", PPlayer->getName(), PPlayer->loc.zone->getName(), PMage->getName());
                say(PMage, pick(h.seed, { "Not going after all? I'll be off, then.", "Changed your mind? No worries. Take care!", "Guess you won't need me. Safe travels!" }));
                giveBack(h, PMage);
                h.landed = false;
                leaveSoon(h);
                return true;
            }

            switch (h.stage)
            {
                case Hire::Stage::Coming:
                {
                    if (secondsSince(h.since) > rules::kComeWithin)
                    {
                        giveUp(h, PMage, "Sorry, I can't get to you.", "she never reached him");
                        return true;
                    }
                    if (PPlayer->loc.zone == nullptr || PPlayer->loc.zone != PMage->loc.zone || PMage->isDead() ||
                        distance(PPlayer->loc.p, PMage->loc.p) > rules::kBesideYalms)
                    {
                        return true;
                    }
                    ShowInfoFmt("teleport: {} has reached {} in {}", PMage->getName(), PPlayer->getName(), PPlayer->loc.zone->getName());
                    h.since  = timer::now();
                    h.beside = h.beside == timer::time_point{} ? timer::now() : h.beside;
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
                    if (secondsSince(h.beside) > rules::kWaitAtMost)
                    {
                        giveUp(h, PMage, "Sorry, I can't wait any longer.", "he never paid");
                        return true;
                    }
                    if (PPlayer->loc.zone != PMage->loc.zone)
                    {
                        ShowInfoFmt("teleport: {} is no longer beside {}, and comes to him again", PMage->getName(), PPlayer->getName());
                        h.stage = Hire::Stage::Coming;
                        h.since = timer::now();
                    }
                    return true; // his trade moves her on (paid)
                }
                case Hire::Stage::Gathering:
                    gather(h, PPlayer, PMage, PController, *crag);
                    return true;
                case Hire::Stage::Casting:
                {
                    if (castingIt(PMage, *crag))
                    {
                        h.cast = true;
                        return true;
                    }
                    // It has landed when anyone of the party beside her is on his way
                    for (auto* PMember : partyOf(PPlayer))
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
                    ShowInfoFmt("teleport: {}'s Teleport-{} came to nothing ({}), try {} of {}", PMage->getName(), crag->name,
                                h.cast ? "interrupted" : "never started", h.tries, rules::kTriesAtMost);
                    if (!h.cast && inHerLine(PController, *crag))
                    {
                        PController->ClearQueuedOrders("her teleport never started");
                    }
                    if (h.tries >= rules::kTriesAtMost)
                    {
                        giveUp(h, PMage, "Sorry, I can't get the spell off.", "every cast came to nothing");
                        return true;
                    }
                    h.stage         = Hire::Stage::Gathering;
                    h.since         = timer::now();
                    h.gatheredSince = {};
                    return true;
                }
                case Hire::Stage::Landing:
                {
                    // At the crag: her thanks, then out of the party
                    const bool there = PMage->loc.zone != nullptr && static_cast<uint16>(PMage->getZone()) == crag->zone && !PMage->requestedZoneChange;
                    if (!there && secondsSince(h.since) < 60.0)
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
    }

    auto Responders::answers(const uint32 charid, const CCharEntity* PPawn, const uint8 mjob, const uint8 mlvl, const uint8 sjob, const uint8 slvl) const -> bool
    {
        auto* PSpell = crag_ != nullptr ? spellOf(*crag_) : nullptr;
        if (PSpell == nullptr)
        {
            return false;
        }
        if (PPawn != nullptr)
        {
            auto* PChar = const_cast<CCharEntity*>(PPawn);
            return rules::answers(charutils::hasSpell(PChar, crag_->spell) != 0, spell::CanUseSpell(PChar, PSpell), static_cast<uint32>(std::max(0, PPawn->health.mp)),
                                  PSpell->getMPCost());
        }
        // Faded, she stands whole for the invite (world.cpp): her MP is her full MP
        const bool casts = rules::castsAt(PSpell->getJob(static_cast<xi::Job>(mjob)), mlvl, PSpell->getJob(static_cast<xi::Job>(sjob)), slvl);
        return rules::answers(knowers_.contains(charid), casts, std::numeric_limits<uint32>::max(), PSpell->getMPCost());
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
        // A fresh yes names its crag and her price; her open contract's
        // invite after a restart finds them again in her variables, with
        // what he had paid her
        uint8  crag   = log;
        uint32 asking = price;
        uint32 paid   = 0;
        if (fresh)
        {
            PMage->setCharVar(kCragVar, log + 1);
            PMage->setCharVar(kPriceVar, static_cast<int32>(price));
            PMage->setCharVar(kPaidVar, 0);
        }
        else
        {
            if (const int32 kept = PMage->getCharVar(kCragVar); kept > 0 && cragOf(static_cast<uint8>(kept - 1)) != nullptr)
            {
                crag = static_cast<uint8>(kept - 1);
            }
            asking = static_cast<uint32>(std::max(0, PMage->getCharVar(kPriceVar)));
            paid   = static_cast<uint32>(std::max(0, PMage->getCharVar(kPaidVar)));
        }
        if (cragOf(crag) == nullptr)
        {
            return;
        }
        // A White Mage who sells the teleport holds the crag's gate crystal,
        // as the game's porters did: she goes with the party, and the spell
        // plays on her as on him -- without it the game blanks her own
        // animation, and the cast looks like no Teleport of his
        if (const auto crystal = static_cast<xi::KeyItem>(cragOf(crag)->keyItem); !charutils::hasKeyItem(PMage, crystal))
        {
            charutils::addKeyItem(PMage, crystal);
            charutils::SaveKeyItems(PMage);
            ShowInfoFmt("teleport: {} holds the gate crystal for {} now", PMage->getName(), cragOf(crag)->name);
        }
        const auto* PPlayer = zoneutils::GetChar(playerCharID);
        // Two invites sent before the first was answered: the first hire
        // stands, and the second White Mage bows out once she has joined
        Hire hire{ .player  = playerCharID,
                   .mage    = mageCharID,
                   .log     = crag,
                   .price   = asking,
                   .paid    = paid,
                   .inTown  = PPlayer != nullptr && isCity(PPlayer->loc.zone),
                   .seed    = xirand::GetRandomNumber(1000u),
                   .hiredAt = timer::now(),
                   .since   = timer::now() };
        if (const auto it = hires.find(playerCharID); it != hires.end() && it->second.mage != mageCharID)
        {
            ShowInfoFmt("teleport: {} joins {} while {} is hired already, and bows out", PMage->getName(), pawn::seats::nameOf(playerCharID),
                        pawn::seats::nameOf(it->second.mage));
            forget(PMage);
            hire.spare = true;
            spares.push_back(std::move(hire));
            return;
        }
        ShowInfoFmt("teleport: {} is hired by {} for Teleport-{} at {} gil{}{}", PMage->getName(), pawn::seats::nameOf(playerCharID), cragOf(crag)->name,
                    asking, hire.inTown ? ", in town" : "", fresh ? "" : fmt::format(" (her open contract, after a restart; {} paid)", paid));
        hires.insert_or_assign(playerCharID, std::move(hire));
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
            PMage->setCharVar(kPaidVar, static_cast<int32>(h.paid));
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
        if ((hires.empty() && spares.empty()) || now - lastLook < kLookEvery)
        {
            return;
        }
        lastLook = now;
        for (auto it = hires.begin(); it != hires.end();)
        {
            it = step(it->second) ? std::next(it) : hires.erase(it);
        }
        std::erase_if(spares, [](Hire& h) { return !step(h); });
    }
} // namespace pawn::teleport
