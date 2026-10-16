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
#include "role_support.h"
#include "spell_bank.h"
#include "tactics.h"

#include "common/logging.h"
#include "common/timer.h"
#include "common/xirand.h"
#include "entities/battle_entity.h"
#include "entities/char_entity.h"
#include "party.h"
#include "spell.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "weapon_skill.h"

#include <algorithm>
#include <chrono>
#include <list>
#include <map>
#include <tuple>
#include <unordered_map>

namespace pawn::chains
{
    namespace
    {
        // Every player's plan, for his session (skillchain_math.h PlanBook)
        cardian::chains::PlanBook book;

        // (player charid, mob id) -> since when both of his plan's members
        // have had the TP for it with no opening on that mob (stateOf): a
        // chain that should have come by now is not expected any more. By mob,
        // as the plan's state is: a member on another mob reads another state
        std::map<std::pair<uint32, uint32>, double> readySince;

        // charid -> since when she has held her TP for the plan (the hold's
        // safety limit, kPlanHoldSafety): kept here, not on her engine, so the
        // others' readings of her ask the same rule her own weapon skill does
        std::unordered_map<uint32, double> holdSince;

        // charid -> the spare weapon skills she has spent on a mob since she
        // last played her part in the plan there (kSparesPerHold)
        struct Spares
        {
            uint32 mob   = 0;
            uint32 spent = 0;
        };
        std::unordered_map<uint32, Spares> spares;

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

        // player charid -> his Skillchain now: the mob, and the time it
        // lapses if the fight outlasts it (it ends with the mob's death)
        struct Override
        {
            uint32 mob   = 0;
            double until = 0.0;
        };
        std::unordered_map<uint32, Override> overrides;
        constexpr double                     kOverrideLasts = 300.0;

        // player charid -> his cardians' ask, once a session (AskWatch)
        std::unordered_map<uint32, cardian::chains::AskWatch> asks;

        // player charid -> the fight the save line was said in: its mob, and
        // when (a mob's id comes back with its next spawn, minutes later)
        struct Said
        {
            uint32 mob = 0;
            double at  = 0.0;
        };
        std::unordered_map<uint32, Said> savesSaid;
        constexpr double                 kSaveSaidLasts = 180.0;

        // A member's weapon skill damage as it lands (wsLanded), a miss as 0,
        // a running mean over her last eight while the map server runs: per
        // weapon skill, and over all of hers
        struct Mean
        {
            double value = 0.0;
            uint32 count = 0;

            void add(const double x)
            {
                count = std::min<uint32>(count + 1, 8);
                value += (x - value) / static_cast<double>(count);
            }
        };
        std::map<std::pair<uint32, uint16>, Mean> wsMeans;
        std::unordered_map<uint32, Mean>          wsMeansAll;

        // The chain's damage for each point of the closing weapon skill,
        // asked of the server once per closer, mob and chain and kept ten
        // minutes (shareOf): her gear and the mob at that id (its next spawn)
        // may change
        struct Share
        {
            double value = 0.0;
            double at    = 0.0;
        };
        std::map<std::tuple<uint32, uint32, uint8>, Share> shares;
        constexpr std::size_t                               kSharesKept = 256;
        constexpr double                                    kShareFresh = 600.0;

        // The plan on a mob as the tactician reads it, worked out at most
        // once a second for a player and a mob (stateOf): whether both its
        // members play their part there, when each has the TP and the chain
        // can start, its worth, his Skillchain now, and the mob's life
        struct PlanState
        {
            double                     at   = 0.0;
            bool                       live = false;
            std::optional<double>      openerReady;
            std::optional<double>      closerReady;
            std::optional<double>      start;
            bool                       forced   = false;
            bool                       saving   = false;
            bool                       stale    = false;
            double                     lifeLeft = -1.0;
            cardian::chains::Worth     worth;
            cardian::chains::WorthCall call;
        };
        std::map<std::pair<uint32, uint32>, PlanState> states;
        constexpr double                               kStateFresh  = 1.0;
        constexpr double                               kStateForget = 30.0;

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
        // kPlanHoldSafety)
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

        void sweep(const double now)
        {
            std::erase_if(tpClocks, [now](const auto& kv)
                          {
                              return now - kv.second.at > kTpForget;
                          });
            std::erase_if(states, [now](const auto& kv)
                          {
                              return now - kv.second.at > kStateForget;
                          });
        }

        auto sample(CBattleEntity* PMember) -> cardian::chains::TpClock&
        {
            const double now = secondsNow();
            if (now - lastSweep > kSweepEvery)
            {
                lastSweep = now;
                sweep(now);
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

        // Every state kept for a player goes: his plan changed or ended
        void forgetStates(const uint32 owner)
        {
            std::erase_if(states, [owner](const auto& kv)
                          {
                              return kv.first.first == owner;
                          });
            std::erase_if(readySince, [owner](const auto& kv)
                          {
                              return kv.first.first == owner;
                          });
        }

        // His Skillchain now on this mob: called, not lapsed, the mob alive.
        // It is one fight's: once its mob is dead or gone it goes, so the
        // same mob back at its next spawn is a fight of its own
        auto forcedOn(const uint32 owner, const CBattleEntity* PMob) -> bool
        {
            const auto it = overrides.find(owner);
            if (it == overrides.end() || PMob == nullptr)
            {
                return false;
            }
            if (it->second.mob != PMob->id)
            {
                const auto* PForced = dynamic_cast<const CBattleEntity*>(zoneutils::GetEntity(it->second.mob, TYPE_MOB));
                if (PForced == nullptr || PForced->isDead())
                {
                    overrides.erase(it);
                }
                return false;
            }
            if (PMob->isDead() || secondsNow() > it->second.until)
            {
                overrides.erase(it);
                return false;
            }
            return true;
        }

        // What her weapon skill deals, as the chain's worth prices it: her
        // landings of it, else of any of hers, else her melee by the
        // formulas for a few rounds (skillchain_math.h kWsPriorRounds)
        auto wsEstimate(CCharEntity* PAsker, CCharEntity* PMember, const uint16 skill, CBattleEntity* PMob) -> double
        {
            if (const auto it = wsMeans.find({ PMember->id, skill }); it != wsMeans.end() && it->second.count > 0)
            {
                return it->second.value;
            }
            if (const auto it = wsMeansAll.find(PMember->id); it != wsMeansAll.end() && it->second.count > 0)
            {
                return it->second.value;
            }
            const auto round = pawn::tactics::meleeRound(PAsker, PMember, PMob);
            return round.has_value() ? *round * cardian::chains::kWsPriorRounds : 0.0;
        }

        // The chain's damage for each point of its closing weapon skill, the
        // server's own formula on this closer and mob (tactics_bank.lua
        // chainShare), kept
        auto shareOf(CCharEntity* PCloser, CBattleEntity* PMob, const uint8 chain) -> double
        {
            const auto   key = std::make_tuple(PCloser->id, PMob->id, chain);
            const double now = secondsNow();
            if (const auto it = shares.find(key); it != shares.end() && now - it->second.at < kShareFresh)
            {
                return it->second.value;
            }
            if (shares.size() >= kSharesKept)
            {
                shares.clear();
            }
            const double share = pawn::tactics::bank::chainShare(PCloser, PMob, chain, tierOf(chain), 1).value_or(0.0);
            shares[key]        = Share{ share, now };
            return share;
        }

        // The best burst on this chain a member of the party fighting this
        // mob would land: one whose tactician casts her nukes -- a cardian,
        // or a played character on autopilot, whose marked nuke row bursts
        // as hers does -- with the MP for one of the chain's elements
        // (CGambits::BurstEstimate). A player bursting by hand is not counted
        auto burstIn(const CCharEntity* PAsker, CBattleEntity* PMob, const uint8 chain) -> double
        {
            if (PAsker == nullptr || PAsker->PParty == nullptr)
            {
                return 0.0;
            }
            const uint16 elements = elementsOf(chain);
            double       best     = 0.0;
            for (auto* PMember : PAsker->PParty->members)
            {
                auto* PChar = dynamic_cast<CCharEntity*>(PMember);
                if (PChar == nullptr || PChar->isDead() || PChar->loc.zone != PAsker->loc.zone)
                {
                    continue;
                }
                if (auto* PGambits = pawn::gambitsOf(PChar); PGambits != nullptr)
                {
                    best = std::max(best, PGambits->BurstEstimate(PMob, elements, 1));
                }
            }
            return best;
        }

        // The plan on this mob, worked out at most once a second for the
        // player (PlanState): read by every member's weapon skill and by the
        // Black Mage's forecast alike, so they agree
        auto stateOf(const uint32 owner, const cardian::chains::Plan& plan, CCharEntity* PAsker, CBattleEntity* PMob) -> const PlanState&
        {
            const double now = secondsNow();
            auto&        s   = states[{ owner, PMob->id }];
            if (s.at > 0.0 && now - s.at < kStateFresh)
            {
                return s;
            }
            s    = PlanState{};
            s.at = now;

            auto* POpener = memberById(PAsker, plan.opener);
            auto* PCloser = memberById(PAsker, plan.closer);
            s.live        = playsPart(POpener, PMob, plan.openerSkill) && playsPart(PCloser, PMob, plan.closerSkill);
            const std::pair<uint32, uint32> key{ owner, PMob->id };
            if (!s.live)
            {
                readySince.erase(key);
                return s;
            }

            // The TP window: when each has the TP, and so when it can start
            const cardian::chains::Ready opener{ POpener->health.tp, rateOf(POpener) };
            const cardian::chains::Ready closer{ PCloser->health.tp, rateOf(PCloser) };
            s.openerReady = cardian::chains::timeToTp(opener.tp, opener.rate);
            s.closerReady = cardian::chains::timeToTp(closer.tp, closer.rate);
            s.start       = cardian::chains::startIn({ opener, closer });
            s.forced      = forcedOn(owner, PMob);

            // Its worth on this mob: what it deals, a burst with it, against
            // what the mob will have left as it lands
            const auto pace        = pawn::tactics::fightPace(PAsker, PMob);
            s.lifeLeft             = pace.has_value() ? pace->remaining : -1.0;
            s.worth.openerWs       = wsEstimate(PAsker, POpener, plan.openerSkill, PMob);
            s.worth.closerWs       = wsEstimate(PAsker, PCloser, plan.closerSkill, PMob);
            s.worth.chainShare     = shareOf(PCloser, PMob, plan.chain);
            s.worth.burst          = burstIn(PAsker, PMob, plan.chain);
            s.worth.hp             = PMob->health.hp;
            s.worth.dealtPerSecond = pace.has_value() ? pace->dealtPerSecond : 0.0;
            s.worth.startIn        = s.start.value_or(0.0);
            s.call                 = cardian::chains::worth(s.worth);
            s.saving               = !s.forced && s.start.has_value() && !s.call.fits;

            // Both in the window longer than the opening takes, and none
            // came: the plan is not happening, whatever its TP says. Not
            // counted while saving, nor while an opening is on the mob,
            // which the opener waits out (wsCall)
            const auto r       = resonanceOn(PMob);
            const bool opening = r.has_value() && r->tier == 0 && r->left > cardian::chains::kWsLands;
            if (!s.saving && !opening && opener.tp >= cardian::chains::kWsTp && closer.tp >= cardian::chains::kWsTp)
            {
                const auto since = readySince.try_emplace(key, now).first->second;
                s.stale          = now - since > *cardian::chains::chainIn(0.0, 0.0);
            }
            else
            {
                readySince.erase(key);
            }
            return s;
        }

        auto heldFor(const CBattleEntity* PMember) -> double
        {
            const auto it = holdSince.find(PMember->id);
            return it != holdSince.end() ? secondsNow() - it->second : 0.0;
        }

        // Whether a member with a weapon skill's TP, fighting this mob, would
        // close what is on it, as her own weapon skill decides (wsCall step
        // 1): a weapon skill of hers chains with it, and a member of the plan
        // playing her part does not keep her TP for the plan instead
        // (keepsForPlan). The chain she would make; 0 for none
        auto closeBy(CCharEntity* PChar, CBattleEntity* PMob, const cardian::chains::Resonance& r) -> uint8
        {
            if (PChar == nullptr || PChar->health.tp < cardian::chains::kWsTp || !fights(PChar, PMob))
            {
                return 0;
            }
            uint8 best = 0;
            for (const auto& sk : skillsOf(PChar))
            {
                const uint8 chain = forms(r.props, sk.props);
                if (chain != 0 && (best == 0 || tierOf(chain) > tierOf(best)))
                {
                    best = chain;
                }
            }
            if (best == 0)
            {
                return 0;
            }
            const auto part     = partOf(PChar, PMob);
            const bool inPlan   = part.seat != cardian::chains::Seat::None && part.partnerFights && usable(PChar, part.skill);
            const bool plansOwn = part.seat == cardian::chains::Seat::Closer && r.tier == 0 && r.props == part.planOpening &&
                                  forms(r.props, propsOf(part.skill)) != 0;
            if (cardian::chains::keepsForPlan({ inPlan, plansOwn, part.spareIn.has_value(), part.saving, part.planComing, heldFor(PChar) }))
            {
                return 0;
            }
            // The plan's own close is made with the plan's weapon skill
            return plansOwn ? forms(r.props, propsOf(part.skill)) : best;
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
        return book.find(ownerCharID);
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
            if (book.clear(POwner->id))
            {
                ShowInfoFmt("chains: {} takes his skillchain plan back", POwner->getName());
            }
            forgetStates(POwner->id);
            overrides.erase(POwner->id);
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
        book.set(POwner->id, Plan{ opener, openerSkill, closer, closerSkill, chain });
        forgetStates(POwner->id);
        overrides.erase(POwner->id);
        auto*       POpenSkill  = battleutils::GetWeaponSkill(openerSkill);
        auto*       PCloseSkill = battleutils::GetWeaponSkill(closerSkill);
        ShowInfoFmt("chains: {} picks {}: {}'s {}, then {}'s {}", POwner->getName(), cardian::chains::chainName(chain), POpener->getName(),
                    POpenSkill != nullptr ? POpenSkill->getName() : "?", PCloser->getName(), PCloseSkill != nullptr ? PCloseSkill->getName() : "?");
        return CL_S_OK;
    }

    auto skillchainNow(CCharEntity* POwner) -> Forced
    {
        Forced out;
        const auto plan = POwner != nullptr ? book.find(POwner->id) : std::nullopt;
        if (!plan.has_value())
        {
            out.status = CL_S_NO_PLAN;
            return out;
        }
        // The mob being fought: the one where both the plan's members play
        // their part -- he may be on an add while they fight the main mob --
        // else his own, else his plan's opener's or closer's
        const auto engagedOn = [](CBattleEntity* PMember) -> CBattleEntity*
        {
            if (PMember == nullptr || PMember->isDead() || PMember->PAI == nullptr || !PMember->PAI->IsEngaged())
            {
                return nullptr;
            }
            auto* PMob = PMember->GetBattleTarget();
            return PMob != nullptr && !PMob->isDead() ? PMob : nullptr;
        };
        auto*                             POpener = memberById(POwner, plan->opener);
        auto*                             PCloser = memberById(POwner, plan->closer);
        const std::array<CBattleEntity*, 3> fought{ engagedOn(POwner), engagedOn(POpener), engagedOn(PCloser) };
        CBattleEntity*                    PMob = nullptr;
        for (auto* PFought : fought)
        {
            if (PMob == nullptr && PFought != nullptr && playsPart(POpener, PFought, plan->openerSkill) && playsPart(PCloser, PFought, plan->closerSkill))
            {
                PMob = PFought;
            }
        }
        for (auto* PFought : fought)
        {
            if (PMob == nullptr)
            {
                PMob = PFought;
            }
        }
        if (PMob == nullptr)
        {
            out.status = CL_S_NO_FIGHT;
            return out;
        }
        overrides[POwner->id] = Override{ PMob->id, secondsNow() + kOverrideLasts };
        forgetStates(POwner->id);
        ShowInfoFmt("chains: {} calls Skillchain now on {}: the plan's {} goes whatever its worth", POwner->getName(), PMob->getName(),
                    cardian::chains::chainName(plan->chain));
        out.status = CL_S_OK;
        out.PMob   = PMob;
        return out;
    }

    void memberLeft(const CBattleEntity* PMember)
    {
        if (PMember == nullptr)
        {
            return;
        }
        for (const uint32 owner : book.memberLeft(PMember->id))
        {
            ShowInfoFmt("chains: a skillchain plan ends: {} left the party", PMember->getName());
            forgetStates(owner);
            overrides.erase(owner);
        }
        tpClocks.erase(PMember->id);
        holdSince.erase(PMember->id);
        spares.erase(PMember->id);
    }

    void mobDied(const uint32 mobId)
    {
        // His Skillchain now was that fight's: the same mob at its next spawn
        // is a fight of its own, and so is the save line's once a fight
        std::erase_if(overrides, [mobId](const auto& kv)
                      {
                          return kv.second.mob == mobId;
                      });
        std::erase_if(savesSaid, [mobId](const auto& kv)
                      {
                          return kv.second.mob == mobId;
                      });
        std::erase_if(states, [mobId](const auto& kv)
                      {
                          return kv.first.second == mobId;
                      });
        std::erase_if(readySince, [mobId](const auto& kv)
                      {
                          return kv.first.second == mobId;
                      });
    }

    void noteHold(const CBattleEntity* PMember, const bool holding)
    {
        if (PMember == nullptr)
        {
            return;
        }
        if (!holding)
        {
            holdSince.erase(PMember->id);
            return;
        }
        holdSince.try_emplace(PMember->id, secondsNow());
    }

    auto heldForPlan(const CBattleEntity* PMember) -> double
    {
        return PMember != nullptr ? heldFor(PMember) : 0.0;
    }

    void noteSpent(const CBattleEntity* PMember, const CBattleEntity* PMob, const bool spare, const bool plays)
    {
        if (PMember == nullptr || PMob == nullptr)
        {
            return;
        }
        auto& mine = spares[PMember->id];
        if (mine.mob != PMob->id || plays)
        {
            mine = Spares{ PMob->id, 0 };
        }
        if (spare)
        {
            ++mine.spent;
        }
    }

    void signedOut(const uint32 playerCharID)
    {
        if (book.clear(playerCharID))
        {
            ShowInfoFmt("chains: a skillchain plan ends with its player's session ({})", playerCharID);
        }
        forgetStates(playerCharID);
        overrides.erase(playerCharID);
        asks.erase(playerCharID);
        savesSaid.erase(playerCharID);
        holdSince.erase(playerCharID);
        spares.erase(playerCharID);
    }

    auto partOf(CCharEntity* PMember, CBattleEntity* PMob) -> Part
    {
        Part       part;
        const auto owner = ownerOf(PMember);
        const auto plan  = PMember != nullptr ? planOf(owner) : std::nullopt;
        if (!plan.has_value() || PMob == nullptr)
        {
            return part;
        }
        // What every member knows of it: its chain, what its opening looks
        // like on the mob, whether its closer will close it, and the plan on
        // her mob as the tactician reads it
        part.planned     = true;
        part.chain       = plan->chain;
        part.planOpening = propsOf(plan->openerSkill);
        if (auto* PCloser = memberById(PMember, plan->closer); PCloser != nullptr)
        {
            part.planCloser       = PCloser->getName();
            part.planCloserFights = playsPart(PCloser, PMob, plan->closerSkill);
        }
        const auto& state = stateOf(owner, *plan, PMember, PMob);
        part.forced       = state.live && state.forced;
        part.saving       = state.live && state.saving;
        part.planComing   = state.live && !state.saving && state.start.has_value();
        if (part.planComing && !state.stale)
        {
            part.startIn = state.start;
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
        if (auto* PPartner = memberById(PMember, partnerId); PPartner != nullptr)
        {
            // Her partner is in this fight with his part of the plan: on her
            // mob, and the plan's weapon skill his with the weapon in his hand
            const uint16 hisSkill = part.seat == cardian::chains::Seat::Opener ? plan->closerSkill : plan->openerSkill;
            part.partner          = PPartner->getName();
            part.partnerFights    = playsPart(PPartner, PMob, hisSkill);
            part.partnerReady     = PPartner->health.tp >= cardian::chains::kWsTp;
        }
        // A weapon skill of hers alone now is spare when she is back in the
        // window before her TP is wanted: the chain's start; or with the
        // chain saved, the mob's end, and only when holding would waste TP at
        // the cap. Never under his Skillchain now, whose members open and
        // close as soon as they can, and never past kSparesPerHold on this
        // mob since she last played her part
        const auto mine     = spares.find(PMember->id);
        const bool sparesOk = mine == spares.end() || mine->second.mob != PMob->id || mine->second.spent < cardian::chains::kSparesPerHold;
        if (state.live && part.partnerFights && !part.forced && sparesOk && PMember->health.tp >= cardian::chains::kWsTp)
        {
            const cardian::chains::Ready her{ PMember->health.tp, rateOf(PMember) };
            if (part.saving)
            {
                part.spareIn = cardian::chains::saveSpare(her, state.lifeLeft >= 0.0 ? std::optional<double>(state.lifeLeft) : std::nullopt);
            }
            else if (!state.stale)
            {
                part.spareIn = cardian::chains::spareWs(her, state.start);
            }
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
            if (PChar != nullptr && PChar != PSelf && closeBy(PChar, PMob, r) != 0)
            {
                return true;
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

    void wsLanded(const CBattleEntity* PMember, const uint16 skill, const int32 damage)
    {
        if (PMember == nullptr || skill == 0 || damage < 0)
        {
            return;
        }
        wsMeans[{ PMember->id, skill }].add(static_cast<double>(damage));
        wsMeansAll[PMember->id].add(static_cast<double>(damage));
    }

    void askIfDue(CCharEntity* PSpeaker, CBattleEntity* PMob)
    {
        // Only a cardian speaks: a played character's party chat is his own
        if (PSpeaker == nullptr || PMob == nullptr || PSpeaker->PSession != nullptr || PSpeaker->PParty == nullptr)
        {
            return;
        }
        const uint32 owner = ownerOf(PSpeaker);
        if (owner == 0 || book.find(owner).has_value())
        {
            return;
        }
        // Settled into this fight: as long as she has fought, as sampled
        const auto   clock     = tpClocks.find(PSpeaker->id);
        const double secondsIn = clock != tpClocks.end() && clock->second.clock.since >= 0.0 ? secondsNow() - clock->second.clock.since : 0.0;
        if (!asks[owner].due(PMob->id, secondsIn))
        {
            return;
        }
        // The party can make one: the options his Shortcuts menu would list
        auto* POwner = zoneutils::GetChar(owner);
        if (POwner == nullptr || POwner->PParty != PSpeaker->PParty || POwner->loc.zone != PSpeaker->loc.zone || optionsFor(POwner).empty())
        {
            return;
        }
        asks[owner].said = true;
        const auto line  = cardian::chains::kAskLines[xirand::GetRandomNumber(cardian::chains::kAskLines.size())];
        ShowInfoFmt("chains: {} asks {}'s party for a skillchain plan: \"{}\"", PSpeaker->getName(), POwner->getName(), line);
        pawn::tactics::role::sayParty(PSpeaker, std::string(line));
    }

    void saySaving(CCharEntity* PMember, CBattleEntity* PMob)
    {
        if (PMember == nullptr || PMob == nullptr || PMember->PSession != nullptr || PMember->PParty == nullptr)
        {
            return;
        }
        const uint32 owner = ownerOf(PMember);
        const auto   plan  = book.find(owner);
        if (owner == 0 || !plan.has_value())
        {
            return;
        }
        const double now  = secondsNow();
        auto&        said = savesSaid[owner];
        if (said.mob == PMob->id && now - said.at < kSaveSaidLasts)
        {
            return;
        }
        said = Said{ PMob->id, now };
        if (const auto it = states.find({ owner, PMob->id }); it != states.end())
        {
            const auto& s = it->second;
            ShowInfoFmt("chains: {}'s party saves {} for the next fight: ~{:.0f} of it ({:.0f} and {:.0f} the weapon skills, x{:.2f} the chain, {:.0f} a burst) "
                        "against ~{:.0f} HP left on {} as it would land in ~{:.0f} s",
                        PMember->getName(), cardian::chains::chainName(plan->chain), s.call.total, s.worth.openerWs, s.worth.closerWs, s.worth.chainShare,
                        s.worth.burst, s.call.hpThen, PMob->getName(), s.call.landsIn);
        }
        const auto line = cardian::chains::kSaveLines[xirand::GetRandomNumber(cardian::chains::kSaveLines.size())];
        pawn::tactics::role::sayParty(PMember, std::string(line));
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
            const auto& state = stateOf(owner, *plan, PNuker, PMob);
            if (!state.live)
            {
                return std::nullopt;
            }
            auto* PCloser = memberById(PNuker, plan->closer);
            // An opening the closer closes: as soon as its window opens and
            // the closer has the TP, making the chain her weapon skill makes
            // on it. The plan's own opening, with the plan's weapon skill;
            // another's only when her own weapon skill would close it -- with
            // the plan on, only on her spare TP (closeBy, keepsForPlan)
            uint8 closes = 0;
            if (opening && resonance->props == propsOf(plan->openerSkill))
            {
                closes = forms(resonance->props, propsOf(plan->closerSkill));
            }
            else if (opening && PCloser != nullptr && PCloser->health.tp >= cardian::chains::kWsTp)
            {
                closes = closeBy(PCloser, PMob, *resonance);
            }
            if (closes != 0)
            {
                const auto ready = state.closerReady;
                if (!ready.has_value() || *ready > resonance->left - cardian::chains::kWsLands)
                {
                    return std::nullopt;
                }
                Forecast f;
                f.chain    = closes;
                f.elements = elementsOf(closes);
                f.in       = std::max(cardian::chains::closeIn(resonance->age), *ready + cardian::chains::kWsLands);
                f.how      = fmt::format("an opening on {} that {} closes", PMob->getName(), PCloser != nullptr ? PCloser->getName() : "the closer");
                return f;
            }
            // Not opened yet: none on a mob it does not fit, where its
            // members keep their TP for the next fight (his Skillchain now
            // aside); none once it is overdue
            if (state.saving || state.stale)
            {
                return std::nullopt;
            }
            Forecast f;
            f.chain    = plan->chain;
            f.elements = elementsOf(plan->chain);
            // Both with the TP; and an opening on the mob that is not the
            // plan's is waited out (wsCall: nobody breaks it)
            auto openerReady = state.openerReady;
            if (opening && openerReady.has_value())
            {
                openerReady = std::max(*openerReady, resonance->left);
            }
            f.in = cardian::chains::chainIn(openerReady, state.closerReady);
            if (!f.in.has_value())
            {
                return std::nullopt;
            }
            auto* POpener = memberById(PNuker, plan->opener);
            f.how         = fmt::format("{} at {} TP and {} at {} TP{}", POpener != nullptr ? POpener->getName() : "?", POpener != nullptr ? POpener->health.tp : 0,
                                        PCloser != nullptr ? PCloser->getName() : "?", PCloser != nullptr ? PCloser->health.tp : 0,
                                        state.forced ? ", Skillchain now" : "");
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
            auto*       PChar = dynamic_cast<CCharEntity*>(PMember);
            const uint8 best  = PChar != nullptr ? closeBy(PChar, PMob, *resonance) : 0;
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
