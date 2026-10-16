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

#include "skillchains.h"

#include "cardian_link_messages.h"
#include "pawn.h"
#include "pawn_gambits.h"
#include "pawn_spellbook.h"
#include "spell_bank.h"
#include "tactics.h"

#include "common/logging.h"
#include "common/timer.h"
#include "entities/battle_entity.h"
#include "entities/char_entity.h"
#include "party.h"
#include "spell.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "weapon_skill.h"

#include <algorithm>
#include <chrono>
#include <list>
#include <unordered_map>

namespace pawn::chains
{
    namespace
    {
        // player charid -> his plan
        std::unordered_map<uint32, Plan> plans;

        // player charid -> since when both of his plan's members have had
        // the TP for it with no opening on their mob (forecast): a chain that
        // should have come by now is not expected any more
        std::unordered_map<uint32, double> readySince;

        // charid -> her TP as sampled (skillchain_math.h TpClock), and when
        // she was last sampled in a fight: one not fought with for a minute
        // is forgotten, swept every few seconds where clocks are sampled
        struct Sampled
        {
            cardian::chains::TpClock clock;
            double                   at = 0.0;
        };
        std::unordered_map<uint32, Sampled> tpClocks;
        constexpr double                    kTpForget   = 60.0;
        constexpr double                    kSweepEvery = 10.0;
        double                              lastSweep   = 0.0;

        auto secondsNow() -> double
        {
            return std::chrono::duration<double>(timer::now().time_since_epoch()).count();
        }

        auto listOf(const cardian::chains::Properties& props) -> std::list<SKILLCHAIN_ELEMENT>
        {
            return { static_cast<SKILLCHAIN_ELEMENT>(props[0]), static_cast<SKILLCHAIN_ELEMENT>(props[1]), static_cast<SKILLCHAIN_ELEMENT>(props[2]) };
        }

        auto propsOf(const uint16 skill) -> cardian::chains::Properties
        {
            const auto* PSkill = battleutils::GetWeaponSkill(skill);
            if (PSkill == nullptr)
            {
                return {};
            }
            return { PSkill->getPrimarySkillchain(), PSkill->getSecondarySkillchain(), PSkill->getTertiarySkillchain() };
        }

        // Who a member's plan is: a player's own; a cardian's, the player
        // whose orders she follows
        auto ownerOf(const CCharEntity* PMember) -> uint32
        {
            if (PMember == nullptr)
            {
                return 0;
            }
            return PMember->PSession != nullptr ? PMember->id : pawn::ordersOwnerOf(PMember);
        }

        // A member of her party by charid, in her zone; nullptr for none
        auto memberById(const CCharEntity* PSelf, const uint32 id) -> CCharEntity*
        {
            if (PSelf == nullptr || id == 0)
            {
                return nullptr;
            }
            if (PSelf->id == id)
            {
                return const_cast<CCharEntity*>(PSelf);
            }
            if (PSelf->PParty == nullptr)
            {
                return nullptr;
            }
            for (auto* PMember : PSelf->PParty->members)
            {
                if (auto* PChar = dynamic_cast<CCharEntity*>(PMember); PChar != nullptr && PChar->id == id && PChar->loc.zone == PSelf->loc.zone)
                {
                    return PChar;
                }
            }
            return nullptr;
        }

        // A weapon skill of hers she can use now: learned, with the weapon in her hand
        auto usable(CCharEntity* PChar, const uint16 skill) -> bool
        {
            return PChar != nullptr && charutils::hasWeaponSkill(PChar, skill) && charutils::canUseWeaponSkill(PChar, skill);
        }

        // A member fighting this mob: alive, engaged on it
        auto fights(const CBattleEntity* PMember, const CBattleEntity* PMob) -> bool
        {
            return PMember != nullptr && PMob != nullptr && !PMember->isDead() && PMember->PAI != nullptr && PMember->PAI->IsEngaged() &&
                   PMember->GetBattleTarget() == PMob;
        }

        // A member who follows a plan: a cardian whose tactician's Weapon
        // skill (best) runs, her gambits on (a plain weapon skill row fires
        // whatever the plan says); a player, who plays by hand, always --
        // the plan's holds are bounded for him (skillchain_math.h
        // kPlanHoldLimit)
        auto follows(CCharEntity* PChar) -> bool
        {
            if (PChar == nullptr)
            {
                return false;
            }
            if (PChar->PSession != nullptr)
            {
                return true;
            }
            auto* PGambits = pawn::gambitsOf(PChar);
            return PGambits != nullptr && PGambits->OffersWeaponSkill();
        }

        // A member of the plan who will play her part on this mob: she
        // fights it, follows the plan, and the plan's weapon skill is hers
        auto playsPart(CCharEntity* PChar, const CBattleEntity* PMob, const uint16 skill) -> bool
        {
            return fights(PChar, PMob) && follows(PChar) && usable(PChar, skill);
        }

        // The members the options read (optionsFor): the player first, then
        // his party in its order
        auto optionMembers(CCharEntity* POwner) -> std::vector<CCharEntity*>
        {
            std::vector<CCharEntity*> out;
            if (POwner == nullptr)
            {
                return out;
            }
            out.push_back(POwner);
            if (POwner->PParty == nullptr)
            {
                return out;
            }
            for (auto* PMember : POwner->PParty->members)
            {
                auto* PChar = dynamic_cast<CCharEntity*>(PMember);
                if (PChar == nullptr || PChar == POwner || PChar->loc.zone != POwner->loc.zone)
                {
                    continue;
                }
                if (PChar->PSession != nullptr)
                {
                    out.push_back(PChar);
                    continue;
                }
                if (auto* PGambits = pawn::gambitsOf(PChar); PGambits != nullptr && PGambits->OffersWeaponSkill())
                {
                    out.push_back(PChar);
                }
            }
            return out;
        }

        // The elements a member can burst with: her tactician's nukes; a real
        // player's, every nuke he has learned at his jobs and level
        auto burstsOf(CCharEntity* PChar) -> uint16
        {
            uint16 bits  = 0;
            auto   nukes = pawn::tactics::nukesOf(PChar);
            if (nukes.empty() && PChar->PSession != nullptr)
            {
                for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
                {
                    auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
                    if (PSpell != nullptr && charutils::hasSpell(PChar, id) && pawn::tactics::bank::isNuke(PSpell) && CSpellBook::Eligible(PChar, PSpell))
                    {
                        nukes.push_back(static_cast<SpellID>(id));
                    }
                }
            }
            for (const auto id : nukes)
            {
                if (auto* PSpell = spell::GetSpell(id); PSpell != nullptr)
                {
                    bits |= cardian::chains::elementBit(static_cast<uint8>(PSpell->getElement()));
                }
            }
            return bits;
        }

        void forgetStaleClocks(const double now)
        {
            std::erase_if(tpClocks, [now](const auto& kv)
                          {
                              return now - kv.second.at > kTpForget;
                          });
        }

        auto sample(CBattleEntity* PMember) -> cardian::chains::TpClock&
        {
            const double now = secondsNow();
            if (now - lastSweep > kSweepEvery)
            {
                lastSweep = now;
                forgetStaleClocks(now);
            }
            const bool fighting = PMember->PAI != nullptr && PMember->PAI->IsEngaged() && !PMember->isDead();
            auto&      s        = tpClocks[PMember->id];
            s.clock.note(PMember->health.tp, now, fighting);
            if (fighting)
            {
                s.at = now;
            }
            return s.clock;
        }

        // Her TP rate believed: measured, else the server's TP for one hit
        // by her weapon's delay, as if every round landed one
        auto rateOf(CCharEntity* PMember) -> double
        {
            const auto& clock = sample(PMember);
            if (clock.measured >= cardian::chains::kTpTrusted)
            {
                return clock.rate;
            }
            const auto delay = PMember->GetWeaponDelay(false);
            if (delay == 0)
            {
                return clock.rate;
            }
            const double perHit = battleutils::CalculateTPFromDamageDealt(PMember, false, SLOT_MAIN);
            return clock.rateOr(perHit * 1000.0 / static_cast<double>(delay));
        }
    } // namespace

    auto forms(const cardian::chains::Properties& opening, const cardian::chains::Properties& closing) -> uint8
    {
        return static_cast<uint8>(battleutils::FormSkillchain(listOf(opening), listOf(closing)));
    }

    auto tierOf(const uint8 chain) -> uint8
    {
        return chain <= SC_DARKNESS_II ? battleutils::GetSkillchainTier(static_cast<SKILLCHAIN_ELEMENT>(chain)) : 0;
    }

    auto elementsOf(const uint8 chain) -> uint16
    {
        if (chain == 0 || chain > SC_DARKNESS_II)
        {
            return 0;
        }
        uint16 bits = 0;
        for (const auto element : battleutils::GetSkillchainMagicElement(static_cast<SKILLCHAIN_ELEMENT>(chain)))
        {
            bits |= cardian::chains::elementBit(static_cast<uint8>(element));
        }
        return bits;
    }

    auto skillsOf(CCharEntity* PChar) -> std::vector<cardian::chains::Skill>
    {
        std::vector<cardian::chains::Skill> out;
        if (PChar == nullptr)
        {
            return out;
        }
        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            if (!charutils::hasWeaponSkill(PChar, id) || !charutils::canUseWeaponSkill(PChar, id))
            {
                continue;
            }
            const auto props = propsOf(id);
            if (props[0] == 0 && props[1] == 0 && props[2] == 0)
            {
                continue;
            }
            out.push_back({ id, props });
        }
        return out;
    }

    auto resonanceOn(CBattleEntity* PMob) -> std::optional<cardian::chains::Resonance>
    {
        auto* PEffect = PMob != nullptr ? PMob->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain) : nullptr;
        if (PEffect == nullptr)
        {
            return std::nullopt;
        }
        cardian::chains::Resonance r;
        const uint16               power = PEffect->GetPower();
        r.tier                           = static_cast<uint8>(PEffect->GetTier());
        if (r.tier == 0)
        {
            // An opening holds its weapon skill's three properties, a nibble each
            r.props = { static_cast<uint8>(power & 0xF), static_cast<uint8>((power >> 4) & 0xF), static_cast<uint8>((power >> 8) & 0xF) };
        }
        else
        {
            r.props = { static_cast<uint8>(power), 0, 0 };
        }
        const auto now = timer::now();
        r.age          = std::chrono::duration<double>(now - PEffect->GetStartTime()).count();
        r.left         = std::chrono::duration<double>(PEffect->GetStartTime() + PEffect->GetDuration() - now).count();
        return r;
    }

    auto planOf(const uint32 ownerCharID) -> std::optional<Plan>
    {
        const auto it = plans.find(ownerCharID);
        return it != plans.end() ? std::optional<Plan>(it->second) : std::nullopt;
    }

    auto optionsFor(CCharEntity* POwner) -> std::vector<Offered>
    {
        std::vector<Offered> out;
        const auto           members = optionMembers(POwner);
        std::vector<cardian::chains::Member> read;
        for (auto* PChar : members)
        {
            read.push_back({ PChar->id, skillsOf(PChar), burstsOf(PChar) });
        }
        const auto nameOf = [&members](const uint32 id) -> std::string
        {
            for (const auto* PChar : members)
            {
                if (PChar->id == id)
                {
                    return PChar->getName();
                }
            }
            return {};
        };
        for (const auto& o : cardian::chains::options(read, forms, tierOf, elementsOf))
        {
            out.push_back({ o, nameOf(o.opener), nameOf(o.closer), nameOf(o.burster) });
        }
        return out;
    }

    auto choose(CCharEntity* POwner, const uint32 opener, const uint16 openerSkill, const uint32 closer, const uint16 closerSkill) -> uint16
    {
        if (POwner == nullptr)
        {
            return CL_S_NOT_IN_PARTY;
        }
        if (opener == 0)
        {
            if (plans.erase(POwner->id) != 0)
            {
                ShowInfoFmt("chains: {} takes his skillchain plan back", POwner->getName());
            }
            readySince.erase(POwner->id);
            return CL_S_OK;
        }
        const auto members = optionMembers(POwner);
        const auto find    = [&members](const uint32 id) -> CCharEntity*
        {
            const auto it = std::ranges::find_if(members, [id](const CCharEntity* PChar) { return PChar->id == id; });
            return it != members.end() ? *it : nullptr;
        };
        auto* POpener = find(opener);
        auto* PCloser = find(closer);
        if (POpener == nullptr || PCloser == nullptr || POpener == PCloser)
        {
            return CL_S_NOT_IN_PARTY;
        }
        const auto hers = [](CCharEntity* PChar, const uint16 skill)
        {
            const auto skills = skillsOf(PChar);
            return std::ranges::any_of(skills, [skill](const cardian::chains::Skill& s) { return s.id == skill; });
        };
        const uint8 chain = hers(POpener, openerSkill) && hers(PCloser, closerSkill) ? forms(propsOf(openerSkill), propsOf(closerSkill)) : 0;
        if (chain == 0)
        {
            return CL_S_NO_SKILLCHAIN;
        }
        plans[POwner->id] = Plan{ opener, openerSkill, closer, closerSkill, chain };
        readySince.erase(POwner->id);
        auto*       POpenSkill  = battleutils::GetWeaponSkill(openerSkill);
        auto*       PCloseSkill = battleutils::GetWeaponSkill(closerSkill);
        ShowInfoFmt("chains: {} picks {}: {}'s {}, then {}'s {}", POwner->getName(), cardian::chains::chainName(chain), POpener->getName(),
                    POpenSkill != nullptr ? POpenSkill->getName() : "?", PCloser->getName(), PCloseSkill != nullptr ? PCloseSkill->getName() : "?");
        return CL_S_OK;
    }

    void memberLeft(const CBattleEntity* PMember)
    {
        if (PMember == nullptr)
        {
            return;
        }
        for (auto it = plans.begin(); it != plans.end();)
        {
            if (it->first == PMember->id || it->second.opener == PMember->id || it->second.closer == PMember->id)
            {
                ShowInfoFmt("chains: a skillchain plan ends: {} left the party", PMember->getName());
                readySince.erase(it->first);
                it = plans.erase(it);
            }
            else
            {
                ++it;
            }
        }
        tpClocks.erase(PMember->id);
    }

    auto partOf(CCharEntity* PMember, CBattleEntity* PMob) -> Part
    {
        Part       part;
        const auto plan = PMember != nullptr ? planOf(ownerOf(PMember)) : std::nullopt;
        if (!plan.has_value())
        {
            return part;
        }
        // What every member knows of it: what its opening looks like on the
        // mob, and whether its closer will close it
        part.planOpening = propsOf(plan->openerSkill);
        if (auto* PCloser = memberById(PMember, plan->closer); PCloser != nullptr)
        {
            part.planCloser       = PCloser->getName();
            part.planCloserFights = playsPart(PCloser, PMob, plan->closerSkill);
        }
        uint32 partnerId = 0;
        if (plan->opener == PMember->id)
        {
            part.seat  = cardian::chains::Seat::Opener;
            part.skill = plan->openerSkill;
            partnerId  = plan->closer;
        }
        else if (plan->closer == PMember->id)
        {
            part.seat  = cardian::chains::Seat::Closer;
            part.skill = plan->closerSkill;
            partnerId  = plan->opener;
        }
        else
        {
            return part;
        }
        part.chain = plan->chain;
        if (auto* PPartner = memberById(PMember, partnerId); PPartner != nullptr)
        {
            // Her partner is in this fight with his part of the plan: on her
            // mob, and the plan's weapon skill his with the weapon in his hand
            const uint16 hisSkill = part.seat == cardian::chains::Seat::Opener ? plan->closerSkill : plan->openerSkill;
            part.partner          = PPartner->getName();
            part.partnerFights    = playsPart(PPartner, PMob, hisSkill);
            part.partnerReady     = PPartner->health.tp >= cardian::chains::kWsTp;
        }
        return part;
    }

    auto othersClose(CCharEntity* PSelf, CBattleEntity* PMob, const cardian::chains::Resonance& r) -> bool
    {
        if (PSelf == nullptr || PSelf->PParty == nullptr)
        {
            return false;
        }
        for (auto* PMember : PSelf->PParty->members)
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            if (PChar == nullptr || PChar == PSelf || PChar->health.tp < cardian::chains::kWsTp || !fights(PChar, PMob))
            {
                continue;
            }
            for (const auto& s : skillsOf(PChar))
            {
                if (forms(r.props, s.props) != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    auto burstComing(CCharEntity* PSelf, const cardian::chains::Resonance& r) -> bool
    {
        if (PSelf == nullptr || r.tier == 0)
        {
            return false;
        }
        const uint16 elements = elementsOf(r.props[0]);
        const auto   bursts   = [&](CCharEntity* PChar)
        {
            return PChar != nullptr && !PChar->isDead() && PChar->loc.zone == PSelf->loc.zone && (burstsOf(PChar) & elements) != 0;
        };
        if (PSelf->PParty == nullptr)
        {
            return bursts(PSelf);
        }
        for (auto* PMember : PSelf->PParty->members)
        {
            if (bursts(dynamic_cast<CCharEntity*>(PMember)))
            {
                return true;
            }
        }
        return false;
    }

    void noteTp(CBattleEntity* PMember)
    {
        // Only a fight's TP is read: out of one, a member with no clock yet
        // gets none (the world's bodies tick too)
        if (PMember == nullptr || ((PMember->PAI == nullptr || !PMember->PAI->IsEngaged()) && !tpClocks.contains(PMember->id)))
        {
            return;
        }
        sample(PMember);
    }

    auto forecast(CCharEntity* PNuker, CBattleEntity* PMob) -> std::optional<Forecast>
    {
        if (PNuker == nullptr || PMob == nullptr)
        {
            return std::nullopt;
        }

        // A chain on the mob now: its burst window is open
        const auto resonance = resonanceOn(PMob);
        if (resonance.has_value() && resonance->tier > 0 && resonance->left > 0.0)
        {
            const auto* PEffect = PMob->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain);
            Forecast    f;
            f.chain    = resonance->props[0];
            f.elements = elementsOf(f.chain);
            f.count    = static_cast<uint8>(std::max<uint16>(1, PEffect != nullptr ? PEffect->GetSubPower() : 1));
            f.open     = resonance->left;
            f.how      = fmt::format("{} is on {}", cardian::chains::chainName(f.chain), PMob->getName());
            return f;
        }
        const bool opening = resonance.has_value() && resonance->tier == 0 && resonance->left > cardian::chains::kWsLands;

        // The plan of the party she fights for, both its members playing
        // their part on her mob
        const uint32 owner = ownerOf(PNuker);
        if (const auto plan = planOf(owner); plan.has_value())
        {
            auto* POpener = memberById(PNuker, plan->opener);
            auto* PCloser = memberById(PNuker, plan->closer);
            if (!playsPart(POpener, PMob, plan->openerSkill) || !playsPart(PCloser, PMob, plan->closerSkill))
            {
                readySince.erase(owner);
                return std::nullopt;
            }
            // An opening the closer's weapon skill closes: as soon as its
            // window opens and the closer has the TP, making the chain his
            // weapon skill makes on it
            const uint8 closes = opening ? forms(resonance->props, propsOf(plan->closerSkill)) : 0;
            if (closes != 0)
            {
                readySince.erase(owner);
                const auto ready = cardian::chains::timeToTp(PCloser->health.tp, rateOf(PCloser));
                if (!ready.has_value() || *ready > resonance->left - cardian::chains::kWsLands)
                {
                    return std::nullopt;
                }
                Forecast f;
                f.chain    = closes;
                f.elements = elementsOf(closes);
                f.in       = std::max(cardian::chains::closeIn(resonance->age), *ready + cardian::chains::kWsLands);
                f.how      = fmt::format("an opening on {} that {} closes", PMob->getName(), PCloser->getName());
                return f;
            }
            // Not opened yet: the opener opens no chain on a mob under half
            // its HP (skillchain_math.h kSpendHpp)
            if (PMob->GetHPP() < cardian::chains::kSpendHpp)
            {
                readySince.erase(owner);
                return std::nullopt;
            }
            // Both have had the TP longer than the opening takes, and none
            // came: the plan is not happening, whatever its TP says. Not
            // counted while another's opening is on the mob, which the
            // opener waits out (wsCall)
            const double now = secondsNow();
            if (!opening && POpener->health.tp >= cardian::chains::kWsTp && PCloser->health.tp >= cardian::chains::kWsTp)
            {
                const auto since = readySince.try_emplace(owner, now).first->second;
                if (now - since > *cardian::chains::chainIn(0.0, 0.0))
                {
                    return std::nullopt;
                }
            }
            else
            {
                readySince.erase(owner);
            }
            Forecast f;
            f.chain    = plan->chain;
            f.elements = elementsOf(plan->chain);
            // Both with the TP; and an opening on the mob that is not the
            // plan's is waited out (wsCall: nobody breaks it)
            auto       openerReady = cardian::chains::timeToTp(POpener->health.tp, rateOf(POpener));
            const auto closerReady = cardian::chains::timeToTp(PCloser->health.tp, rateOf(PCloser));
            if (opening && openerReady.has_value())
            {
                openerReady = std::max(*openerReady, resonance->left);
            }
            f.in = cardian::chains::chainIn(openerReady, closerReady);
            if (!f.in.has_value())
            {
                return std::nullopt;
            }
            f.how = fmt::format("{} at {} TP and {} at {} TP", POpener->getName(), POpener->health.tp, PCloser->getName(), PCloser->health.tp);
            return f;
        }

        // No plan: an opening that a member fighting the mob, with the TP,
        // can close
        if (!opening || PNuker->PParty == nullptr)
        {
            return std::nullopt;
        }
        for (auto* PMember : PNuker->PParty->members)
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            if (PChar == nullptr || PChar->health.tp < cardian::chains::kWsTp || !fights(PChar, PMob))
            {
                continue;
            }
            uint8 best = 0;
            for (const auto& s : skillsOf(PChar))
            {
                const uint8 chain = forms(resonance->props, s.props);
                if (chain != 0 && (best == 0 || tierOf(chain) > tierOf(best)))
                {
                    best = chain;
                }
            }
            if (best != 0)
            {
                Forecast f;
                f.chain    = best;
                f.elements = elementsOf(best);
                f.in       = cardian::chains::closeIn(resonance->age);
                f.how      = fmt::format("an opening {} can close", PChar->getName());
                return f;
            }
        }
        return std::nullopt;
    }
} // namespace pawn::chains
