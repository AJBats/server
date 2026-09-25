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

#include "spell_bank.h"

#include "fight_log.h"
#include "pawn_gambits.h"
#include "pawn_spellbook.h"
#include "tactics.h"

#include "common/logging.h"
#include "common/timer.h"

#include "ai/ai_container.h"
#include "enmity_container.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "recast_container.h"
#include "items/item_weapon.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>

namespace pawn::tactics
{
    namespace
    {
        using namespace cardian::tactics;

        // The effect, the stat it rolls on and the accuracy bonus mirror
        // pTable in scripts/globals/spells/enfeebling_spell.lua (a local
        // there); Dia and Bio read their own spell scripts
        constexpr Priced kPriced[] = {
            { SpellID::Paralyze, Model::StoppedRounds, xi::StatusEffect::Paralysis, 1, xi::Mod::MND, -10, 0, 0.0 },
            { SpellID::Slow, Model::StoppedRounds, xi::StatusEffect::Slow, 3, xi::Mod::MND, 10, 0, 0.0 },
            { SpellID::Blind, Model::Misses, xi::StatusEffect::Blindness, 1, xi::Mod::INT, 0, 0, 0.0 },
            { SpellID::Dia, Model::DefenceDown, xi::StatusEffect::Dia, 1, xi::Mod::INT, 0, 60, 3.0 },
            { SpellID::Diaga, Model::DefenceDown, xi::StatusEffect::Dia, 1, xi::Mod::INT, 0, 60, 3.0 },
            { SpellID::Poison, Model::Dot, xi::StatusEffect::Poison, 1, xi::Mod::INT, 0, 0, 3.0 },
            { SpellID::Poisonga, Model::Dot, xi::StatusEffect::Poison, 1, xi::Mod::INT, 0, 0, 3.0 },
            { SpellID::Bio, Model::Dot, xi::StatusEffect::Bio, 2, xi::Mod::INT, 0, 60, 3.0 }, // the ticks only
        };

        // Her allow-list's lists (tactician_line.h) are the bank's own, in
        // its order, with spell.h's numbers
        static_assert(std::size(kPriced) == cardian::tactician::kPricedDebuffs.size());
        static_assert([]
                      {
                          for (std::size_t i = 0; i < std::size(kPriced); ++i)
                          {
                              if (static_cast<uint16>(kPriced[i].id) != cardian::tactician::kPricedDebuffs[i].id)
                              {
                                  return false;
                              }
                          }
                          return true;
                      }());
        static_assert(cardian::tactician::kPricedDebuffs[0].family == SPELLFAMILY_PARALYZE && cardian::tactician::kPricedDebuffs[1].family == SPELLFAMILY_SLOW &&
                      cardian::tactician::kPricedDebuffs[2].family == SPELLFAMILY_BLIND && cardian::tactician::kPricedDebuffs[3].family == SPELLFAMILY_DIA &&
                      cardian::tactician::kPricedDebuffs[4].family == SPELLFAMILY_DIAGA && cardian::tactician::kPricedDebuffs[5].family == SPELLFAMILY_POISON &&
                      cardian::tactician::kPricedDebuffs[6].family == SPELLFAMILY_POISONGA && cardian::tactician::kPricedDebuffs[7].family == SPELLFAMILY_BIO);
        static_assert(cardian::tactician::kCureFamily == SPELLFAMILY_CURE);
        static_assert(cardian::tactician::kCureTiers[0] == static_cast<uint16>(SpellID::Cure) && cardian::tactician::kCureTiers[1] == static_cast<uint16>(SpellID::Cure_II) &&
                      cardian::tactician::kCureTiers[2] == static_cast<uint16>(SpellID::Cure_III) && cardian::tactician::kCureTiers[3] == static_cast<uint16>(SpellID::Cure_IV) &&
                      cardian::tactician::kCureTiers[4] == static_cast<uint16>(SpellID::Cure_V) && cardian::tactician::kCureTiers[5] == static_cast<uint16>(SpellID::Cure_VI));

        constexpr int         kResistRolls = 100;
        constexpr int         kPdifRolls   = 300;
        constexpr std::size_t kPdifCacheCap = 4096; // entries; cleared when full, a cache and not a leak

        // The defence a defence-down effect takes, in percent, learned from
        // the first one seen on a mob, by effect and tier (Dia, Dia II and
        // Diaga all write the one effect); ten stands in until then
        std::map<std::pair<xi::StatusEffect, uint16>, int32> observedDefenceDown;

        auto defenceDownPercent(const xi::StatusEffect effect, const uint16 tier) -> std::pair<int32, bool> // the percent, and whether it is assumed
        {
            if (const auto it = observedDefenceDown.find({ effect, tier }); it != observedDefenceDown.end())
            {
                return { it->second, false };
            }
            return { 10, true };
        }

        // The mean pDIF a member's melee lands at a defence, sampled once
        // per everything the formula reads and kept for the process: the
        // member, her weapon, attack and level, the zone (level correction),
        // the mob's level and the defence
        struct PdifKey
        {
            uint32 member   = 0;
            uint8  weapon   = 0;
            int32  defence  = 0;
            uint8  mobLevel = 0;
            uint16 attack   = 0;
            uint8  level    = 0;
            uint16 zone     = 0;

            auto operator<=>(const PdifKey&) const = default;
        };
        struct PdifSample
        {
            double mean    = 0.0;
            double biggest = 0.0;
        };
        std::map<PdifKey, PdifSample> pdifCache;

        // The self-check, once per member for the process: the sampler is
        // asked the same question twice and must agree with itself
        std::set<uint32> selfChecked;

        // The samplers, modules/cardian/lua/tactics_bank.lua
        auto bankFunction(const char* name) -> std::optional<sol::protected_function>
        {
            auto bank = ::lua["xi"]["cardian"]["bank"];
            if (!bank.valid())
            {
                static bool said = false;
                if (!said)
                {
                    said = true;
                    ShowError("tactics: bank: modules/cardian/lua/tactics_bank.lua is not loaded; nothing is priced");
                }
                return std::nullopt;
            }
            return sol::protected_function(bank[name]);
        }

        // A formula that failed says so once per name: a question the
        // stand-in cannot answer shows in the log without flooding it
        auto failed(const char* name, const sol::protected_function_result& res) -> bool
        {
            if (res.valid())
            {
                return false;
            }
            static std::set<std::string> said;
            if (said.insert(name).second)
            {
                sol::error err = res;
                ShowError("tactics: bank: {} failed: {}", name, err.what());
            }
            return true;
        }

        // Each effect on the mob carrying the modifier: the effect, its
        // tier and the amount
        struct EffectMod
        {
            xi::StatusEffect effect;
            uint16           tier;
            int32            amount;
        };

        auto effectsWith(CMobEntity* PMob, const xi::Mod mod) -> std::vector<EffectMod>
        {
            std::vector<EffectMod> out;
            PMob->StatusEffectContainer->ForEachEffect([&](CStatusEffect& effect)
                                                       {
                                                           for (const auto& m : effect.modList())
                                                           {
                                                               if (m.getModID() == mod)
                                                               {
                                                                   out.push_back(EffectMod{ effect.GetStatusID(), effect.GetTier(), m.getModAmount() });
                                                               }
                                                           }
                                                       });
            return out;
        }

        auto nameOf(const xi::StatusEffect effect) -> std::string
        {
            return effects::GetEffectName(static_cast<uint16>(effect));
        }

        auto weaponType(CBattleEntity* PActor) -> xi::SkillType
        {
            auto* PWeapon = dynamic_cast<CItemWeapon*>(PActor->m_Weapons[SLOT_MAIN]);
            return PWeapon != nullptr ? PWeapon->getSkillType() : xi::SkillType::HandToHand;
        }

        // The pDIF expectation of one actor against a target of the given
        // level and defence: the mean, and the biggest roll seen
        auto samplePdif(CBattleEntity* PActor, const uint8 targetLevel, const int32 defence, const xi::SkillType type) -> std::optional<PdifSample>
        {
            auto fn = bankFunction("expectedPdif");
            if (!fn)
            {
                return std::nullopt;
            }
            auto res = (*fn)(CLuaBaseEntity(PActor), defence, targetLevel, static_cast<uint8>(type), kPdifRolls);
            if (failed("expectedPdif", res))
            {
                return std::nullopt;
            }
            return PdifSample{ res.get<double>(0), res.get<double>(1) };
        }

        // The cached expectation, or a fresh sample when allowed; nothing
        // when the answer would cost 300 rolls the caller has not budgeted
        auto pdifOf(CBattleEntity* PActor, const uint8 targetLevel, const int32 defence, const xi::SkillType type, const bool allowSample = true) -> std::optional<PdifSample>
        {
            const PdifKey key{ .member = PActor->id, .weapon = static_cast<uint8>(type), .defence = defence, .mobLevel = targetLevel, .attack = PActor->ATT(SLOT_MAIN), .level = PActor->GetMLevel(), .zone = PActor->loc.zone != nullptr ? static_cast<uint16>(PActor->loc.zone->GetID()) : uint16{ 0 } };
            if (const auto it = pdifCache.find(key); it != pdifCache.end())
            {
                return it->second;
            }
            if (!allowSample)
            {
                return std::nullopt;
            }
            const auto sample = samplePdif(PActor, targetLevel, defence, type);
            if (sample)
            {
                if (pdifCache.size() >= kPdifCacheCap)
                {
                    pdifCache.clear();
                }
                pdifCache[key] = *sample;
            }
            return sample;
        }

        auto pdif(CCharEntity* PMember, CMobEntity* PMob, const int32 defence) -> std::optional<double>
        {
            const auto sample = pdifOf(PMember, PMob->GetMLevel(), defence, weaponType(PMember));
            return sample ? std::optional<double>(sample->mean) : std::nullopt;
        }

        // Two independent batches at one defence, their ratio: the
        // sampler's agreement with itself, the user's acceptance test
        auto selfCheck(CCharEntity* PMember, CMobEntity* PMob, const int32 defence) -> std::optional<double>
        {
            const auto type = weaponType(PMember);
            const auto a    = samplePdif(PMember, PMob->GetMLevel(), defence, type);
            const auto b    = samplePdif(PMember, PMob->GetMLevel(), defence, type);
            if (!a || !b || b->mean <= 0.0)
            {
                return std::nullopt;
            }
            return a->mean / b->mean;
        }

        // The land chance, sampled once per caster and spell for the fight;
        // nothing when the sampler failed
        auto land(FightRecord& r, CBattleEntity* PCaster, CMobEntity* PMob, CSpell* PSpell, const Priced& p) -> std::optional<FightRecord::LandChance>
        {
            const auto key = std::pair{ PCaster->id, static_cast<uint16>(p.id) };
            if (const auto it = r.landCache.find(key); it != r.landCache.end())
            {
                return it->second;
            }
            auto fn = bankFunction("landChance");
            if (!fn)
            {
                return std::nullopt;
            }
            // What useEnfeeblingSpell feeds the resist roll, less the magic
            // burst tier nobody here has
            sol::table fed        = ::lua.create_table();
            fed["effectId"]       = static_cast<uint16>(p.effect);
            fed["magicalElement"] = PSpell->getElement();
            fed["actorStat"]      = static_cast<uint16>(p.stat);
            fed["skillType"]      = static_cast<uint8>(PSpell->getSkillType());
            fed["spellGroup"]     = static_cast<uint8>(PSpell->getSpellGroup());
            fed["bonusMacc"]      = p.bonusMacc;
            fed["magicBurstTier"] = 0;
            fed["tier"]           = p.tier;
            auto res              = (*fn)(CLuaBaseEntity(PCaster), CLuaBaseEntity(PMob), fed, p.fixedDuration > 0 ? 0 : kResistRolls);
            if (failed("landChance", res))
            {
                return std::nullopt;
            }
            const FightRecord::LandChance land{ res.get<double>(0), res.get<double>(1) };
            r.landCache[key] = land;
            return land;
        }

        // The mob's defence: the server's own figure, its percentage, and
        // the share of the percentage that is defence-down effects. The
        // percentage scales the figure, so a counterfactual is a scaling too
        struct Defence
        {
            int32                     now  = 1; // CBattleEntity::DEF()
            int32                     defp = 0;
            int32                     down = 0;
            std::vector<DefenceShare> effects;

            auto at(const int32 percent) const -> int32
            {
                if (100 + defp <= 0)
                {
                    return now;
                }
                return std::max(1, static_cast<int32>(std::lround(static_cast<double>(now) * (100 + percent) / (100 + defp))));
            }
            auto base() const -> int32
            {
                return at(defp - down);
            }
        };

        auto defenceOf(CMobEntity* PMob) -> Defence
        {
            Defence d;
            d.now  = PMob->DEF();
            d.defp = PMob->getMod(xi::Mod::DEFP);
            for (const auto& [effect, tier, amount] : effectsWith(PMob, xi::Mod::DEFP))
            {
                if (amount < 0)
                {
                    d.effects.push_back(DefenceShare{ .effect = nameOf(effect), .defp = amount });
                    d.down += amount;
                    observedDefenceDown[{ effect, tier }] = -amount;
                }
            }
            return d;
        }

        // The fight's rates, live once it has ten seconds of its own, the
        // spot's before
        struct Rates
        {
            double dealtPerSecond = 0.0;
            double takenPerSecond = 0.0;
            double meleePerRound  = 0.0;
            double roundDelay     = 0.0;
            double remaining      = -1.0;
            bool   guessed        = true;
            bool   formula        = false; // the formulas' word: nothing measured, nothing on record
        };

        // The mob's melee on its target and the party's melee on it, by the
        // formulas (RESEARCH §12.2 item 5): the prior before the fight or
        // the spot has a number. The mob's target is whoever it is on, else
        // the sturdiest member; the party is every member in the zone
        void priorRates(Rates& x, FightRecord& r, CMobEntity* PMob, const std::vector<CBattleEntity*>* members, const bool wantTaken, const bool wantDealt)
        {
            // One fresh pDIF sample a call: the first think at a new spot
            // gets the party's rate as the samples come in, never all at once
            bool           sampled = false;
            CBattleEntity* PTarget = PMob->GetBattleTarget();
            if (PTarget == nullptr && members != nullptr)
            {
                for (auto* PMember : *members)
                {
                    if (PMember != nullptr && !PMember->isDead() && PMember->loc.zone == PMob->loc.zone && (PTarget == nullptr || PMember->GetMaxHP() > PTarget->GetMaxHP()))
                    {
                        PTarget = PMember;
                    }
                }
            }
            if (wantTaken && PTarget != nullptr)
            {
                const bool before = r.meleeCache.contains({ PMob->id, PTarget->id });
                if (const auto g = bank::melee(r, PMob, PTarget, !sampled); g.has_value())
                {
                    x.takenPerSecond = g->perSecond;
                    x.meleePerRound  = g->perRound;
                    x.formula        = true;
                    if (r.priorTaken < 0.0)
                    {
                        r.priorTaken = g->perSecond;
                    }
                }
                sampled = sampled || (!before && r.meleeCache.contains({ PMob->id, PTarget->id }));
            }
            // The party's melee: whoever does not attend this fight from the
            // perimeter (a Support Mage no Attack row of hers sends onto
            // the mob), who stands back by design; the measured rate
            // replaces the guess at ten seconds either way
            if (wantDealt && members != nullptr)
            {
                double dealt    = 0.0;
                bool   complete = true;
                for (auto* PMember : *members)
                {
                    if (PMember == nullptr || PMember->isDead() || PMember->loc.zone != PMob->loc.zone || attendsFight(PMember, PMob))
                    {
                        continue;
                    }
                    const bool before = r.meleeCache.contains({ PMember->id, PMob->id });
                    const auto g      = bank::melee(r, PMember, PMob, !sampled);
                    sampled           = sampled || (!before && r.meleeCache.contains({ PMember->id, PMob->id }));
                    if (g.has_value())
                    {
                        dealt += g->perSecond;
                    }
                    else if (!r.meleeCache.contains({ PMember->id, PMob->id }))
                    {
                        complete = false;
                    }
                }
                if (dealt > 0.0)
                {
                    x.dealtPerSecond = dealt;
                    x.remaining      = PMob->health.hp / dealt;
                    x.formula        = true;
                    if (complete && r.priorDealt < 0.0)
                    {
                        r.priorDealt = dealt;
                    }
                }
            }
        }

        auto ratesOf(FightRecord& r, const SpotAverages& spot, CMobEntity* PMob, const double now, const std::vector<CBattleEntity*>* members) -> Rates
        {
            Rates        x;
            const double secs      = r.seconds(now);
            const bool   live      = secs >= 10.0;
            const double liveDealt = r.dealtPerSecond(now);
            const double liveTaken = r.takenPerSecond(now);
            int32        tp        = 0;
            for (const auto& m : r.members)
            {
                tp += m.tpMoveDamage;
            }
            const double liveMelee = secs > 0.0 ? (r.taken() - tp) / secs : 0.0;
            x.dealtPerSecond       = live && liveDealt > 0.0 ? liveDealt : spot.dealtPerSecond.mean;
            x.takenPerSecond       = live && liveTaken > 0.0 ? liveTaken : spot.takenPerSecond.mean;
            // The spot's rate counts TP moves too: near enough before the fight has its own
            const double meleePerSecond = live && liveMelee > 0.0 ? liveMelee : spot.takenPerSecond.mean;
            x.roundDelay                = PMob->GetWeaponDelay(false) / 1000.0;
            x.meleePerRound             = meleePerSecond * x.roundDelay;
            x.remaining                 = remainingLife(PMob->health.hp, liveDealt, secs, spot.dealtPerSecond.mean);
            x.guessed                   = !(live && liveDealt > 0.0);

            const bool haveTaken = (live && liveTaken > 0.0) || spot.fights > 0;
            const bool haveDealt = (live && liveDealt > 0.0) || spot.fights > 0;
            if (!haveTaken || !haveDealt)
            {
                priorRates(x, r, PMob, members, !haveTaken, !haveDealt);
            }
            return x;
        }

        auto potencyOf(CBattleEntity* PCaster, CMobEntity* PMob, CSpell* PSpell, const Priced& p) -> std::optional<double>
        {
            switch (p.id)
            {
                case SpellID::Dia:
                case SpellID::Diaga:
                    return 1.0 + PCaster->getMod(xi::Mod::DIA_DOT);
                case SpellID::Bio:
                    return std::clamp(std::ceil(PCaster->GetSkill(xi::SkillType::DarkMagic) / 40.0), 1.0, 3.0);
                default:
                    break;
            }
            sol::protected_function fn  = ::lua["xi"]["spells"]["enfeebling"]["calculatePotency"];
            auto                    res = fn(CLuaBaseEntity(PCaster), CLuaBaseEntity(PMob), static_cast<uint16>(p.id), static_cast<uint16>(p.effect), static_cast<uint8>(PSpell->getSkillType()), static_cast<uint16>(p.stat));
            if (failed("calculatePotency", res))
            {
                return std::nullopt;
            }
            return res.get<double>(0);
        }

        auto durationOf(CBattleEntity* PCaster, CMobEntity* PMob, CSpell* PSpell, const Priced& p) -> std::optional<double>
        {
            if (p.fixedDuration > 0)
            {
                return static_cast<double>(p.fixedDuration);
            }
            sol::protected_function fn  = ::lua["xi"]["spells"]["enfeebling"]["calculateDuration"];
            auto                    res = fn(CLuaBaseEntity(PCaster), CLuaBaseEntity(PMob), static_cast<uint16>(p.id), static_cast<uint16>(p.effect), static_cast<uint8>(PSpell->getSkillType()));
            if (failed("calculateDuration", res))
            {
                return std::nullopt;
            }
            return res.get<double>(0);
        }

        // Seconds an effect has to go; a permanent one (no duration) reads
        // as infinite
        auto secondsLeft(const CStatusEffect* PEffect) -> double
        {
            if (PEffect->GetDuration() == timer::duration::zero())
            {
                return std::numeric_limits<double>::infinity();
            }
            const auto elapsed = timer::now() - PEffect->GetStartTime();
            return std::max(0.0, std::chrono::duration<double>(PEffect->GetDuration() - elapsed).count());
        }

        // The party's melee with the effect on the mob over its melee
        // without: each member with enmity on the mob, weighted by what she
        // has dealt this fight. Before a cast the "with" is the defence
        // lowered by the effect's percent; at a cast line the effect is on
        // already, so "without" is the defence with it lifted
        struct Ratio
        {
            double ratio   = 1.0;
            int32  percent = 0;
            bool   assumed = false; // the percent is the table's, not yet seen on a mob
        };

        // Whose melee the forecast weighs: the scope's members when the list
        // prints at open (the enmity list is empty until the first hit),
        // else whoever holds enmity on the mob; each by what she has dealt
        auto meleeCandidates(const FightRecord& r, CMobEntity* PMob, const std::vector<CBattleEntity*>* members) -> std::vector<std::pair<CCharEntity*, double>>
        {
            std::vector<std::pair<CCharEntity*, double>> out;
            auto add = [&](CBattleEntity* PEntity)
            {
                if (PEntity == nullptr || PEntity->objtype != TYPE_PC)
                {
                    return;
                }
                const auto* figures = r.find(PEntity->id);
                out.emplace_back(static_cast<CCharEntity*>(PEntity), figures != nullptr ? std::max(1, figures->damageDealt) : 1.0);
            };
            if (members != nullptr)
            {
                for (auto* PMember : *members)
                {
                    add(PMember);
                }
            }
            else
            {
                for (const auto& [id, enmity] : *PMob->PEnmityContainer->GetEnmityList())
                {
                    add(enmity.PEnmityOwner);
                }
            }
            return out;
        }

        auto defenceDownRatio(const FightRecord& r, CMobEntity* PMob, const Priced& p, const bool onAlready, const std::vector<CBattleEntity*>* members) -> std::optional<Ratio>
        {
            // defenceOf has just noted every defence-down effect on the mob,
            // so an effect on it already reads its exact percent here
            const auto d                  = defenceOf(PMob);
            const auto [percent, assumed] = defenceDownPercent(p.effect, p.tier);
            const int32 defWith           = onAlready ? d.now : d.at(d.defp - percent);
            const int32 defWithout        = onAlready ? d.at(d.defp + percent) : d.now;
            double      weighted          = 0.0;
            double      weights           = 0.0;
            for (const auto& [PChar, w] : meleeCandidates(r, PMob, members))
            {
                const auto without = pdif(PChar, PMob, defWithout);
                const auto with    = pdif(PChar, PMob, defWith);
                if (!without || !with || *without <= 0.0)
                {
                    continue;
                }
                weighted += w * (*with / *without);
                weights += w;
            }
            if (weights <= 0.0)
            {
                return std::nullopt;
            }
            return Ratio{ weighted / weights, percent, assumed };
        }

        // justCast: the line for a cast the log saw, priced as the spell
        // stood before it -- the effect it put on the mob is there already
        auto priceDebuff(FightRecord& r, const SpotAverages& spot, const Exchange& x, CBattleEntity* PCaster, CMobEntity* PMob, CSpell* PSpell, const Priced& p, const bool justCast, const std::vector<CBattleEntity*>* members) -> DebuffPrice
        {
            DebuffPrice price;
            price.id     = static_cast<uint16>(PSpell->getID());
            price.spell  = PSpell->getName();
            price.target = PMob->getName();
            price.mp     = battleutils::CalculateSpellCost(PCaster, PSpell);

            const auto* PEffect = PMob->StatusEffectContainer->GetStatusEffect(p.effect);
            if (PEffect != nullptr && !justCast)
            {
                price.onFor = secondsLeft(PEffect);
                return price;
            }
            // An effect on the mob that nullifies this one (a Bio under a
            // Dia): the game's own rule, asked before any pricing
            if (PEffect == nullptr)
            {
                sol::protected_function nullified = ::lua["xi"]["data"]["statusEffect"]["isEffectNullified"];
                if (auto res = nullified(CLuaBaseEntity(PMob), static_cast<uint16>(p.effect), p.tier); !failed("isEffectNullified", res) && res.get<bool>(0))
                {
                    price.blocked = true;
                    return price;
                }
            }
            const auto   rt       = ratesOf(r, spot, PMob, seconds(timer::now()), members);
            const double castTime = std::chrono::duration<double>(PSpell->getCastTime()).count();
            price.lifeGuessed     = rt.guessed;
            price.formula         = rt.formula;
            if (rt.remaining >= 0.0 && rt.remaining < castTime + 2.0)
            {
                price.moot = rt.remaining;
                return price;
            }
            const auto chance = land(r, PCaster, PMob, PSpell, p);
            const auto pot    = potencyOf(PCaster, PMob, PSpell, p);
            const auto dur    = durationOf(PCaster, PMob, PSpell, p);
            if (!chance || !pot || !dur)
            {
                price.priced = false;
                price.family = "a formula failed, see the error above";
                return price;
            }
            price.landChance = chance->chance;
            price.duration   = *dur * (p.fixedDuration > 0 ? 1.0 : chance->rate);
            price.window     = window(price.duration, rt.remaining);

            switch (p.model)
            {
                case Model::StoppedRounds:
                {
                    // Paralyze's potency is its proc chance in percent; Slow's
                    // is the delay it adds, in ten-thousandths
                    const double fraction = p.id == SpellID::Paralyze ? *pot / 100.0 : *pot / (10000.0 + *pot);
                    priceRounds(price, rt.roundDelay, fraction, rt.meleePerRound, p.id == SpellID::Paralyze ? "rounds stopped" : "rounds lost");
                    break;
                }
                case Model::Misses:
                {
                    // Blind's potency is the accuracy it takes: the mob's hit
                    // rate on its target with and without it
                    double      fraction = *pot / 200.0;
                    std::string how      = "no target on record, assumed";
                    if (auto* PTarget = PMob->GetBattleTarget(); PTarget != nullptr)
                    {
                        const double hitNow   = battleutils::GetHitRate(PMob, PTarget, 0, 0) / 100.0;
                        const double hitBlind = battleutils::GetHitRate(PMob, PTarget, 0, static_cast<int16>(-*pot)) / 100.0;
                        fraction              = hitNow > 0.0 ? (hitNow - hitBlind) / hitNow : 0.0;
                        how                   = fmt::format("hit rate {:.0f}% -> {:.0f}% on {}", hitNow * 100.0, hitBlind * 100.0, PTarget->getName());
                    }
                    priceRounds(price, rt.roundDelay, fraction, rt.meleePerRound, "rounds missed");
                    price.detail += ", " + how;
                    break;
                }
                case Model::Dot:
                {
                    const double extra = dotDamage(*pot, p.tick, price.window);
                    priceExtraDamage(price, extra, rt.dealtPerSecond, rt.takenPerSecond, rt.remaining, fmt::format("{:.0f} a tick", *pot));
                    break;
                }
                case Model::DefenceDown:
                {
                    // The defence the effect takes (learned from the first one
                    // seen): the party's melee lands harder for the window,
                    // and it ticks
                    const auto ratio = defenceDownRatio(r, PMob, p, PEffect != nullptr, members);
                    if (!ratio)
                    {
                        price.noData = true;
                        price.detail = "nobody meleeing it on record";
                        break;
                    }
                    const double byDefence = defenceDownExtra(ratio->ratio, rt.dealtPerSecond, price.window);
                    const double byTicks   = dotDamage(*pot, p.tick, price.window);
                    priceExtraDamage(price, byDefence + byTicks, rt.dealtPerSecond, rt.takenPerSecond, rt.remaining,
                                     fmt::format("melee x{:.2f} at -{}% defence{} (+{:.0f}), {:.0f} a tick (+{:.0f})", ratio->ratio, ratio->percent, ratio->assumed ? " (assumed)" : "", byDefence, *pot, byTicks));
                    break;
                }
            }
            price.mpWorth = x.mp(price.hpSaved);
            return price;
        }

        // A spell the bank has no model for: its skill and family, so the
        // log shows what the party leans on
        auto familyOf(CSpell* PSpell) -> std::string
        {
            return fmt::format("{}, {}", magic_enum::enum_name(PSpell->getSkillType()), pawn::familyName(static_cast<uint32>(PSpell->getSpellFamily())));
        }
    } // namespace

    auto priced(const SpellID id) -> const Priced*
    {
        for (const auto& p : kPriced)
        {
            if (p.id == id)
            {
                return &p;
            }
        }
        return nullptr;
    }

    namespace bank
    {
        void load()
        {
            const auto res = ::lua.safe_script_file("./modules/cardian/lua/tactics_bank.lua");
            if (!res.valid())
            {
                sol::error err = res;
                ShowError("tactics: bank: the samplers did not load: {}", err.what());
            }
        }

        auto castLine(FightRecord* r, const Exchange& x, CBattleEntity* PCaster, CBattleEntity* PTarget, CSpell* PSpell, const int32 missing) -> std::string
        {
            if (PSpell->isCure() && PTarget != nullptr && asMob(PTarget) == nullptr) // a cure on undead is an attack
            {
                std::vector<CureOption> options;
                for (const auto& tier : cureTiers(PCaster, CureAvailability::Eligible))
                {
                    options.push_back(tier.option);
                }
                const auto  pick = pickCure(options, missing);
                std::string line = cureLine(PCaster->getName(), PTarget->getName(), missing, options, pick);
                if (pick != kNoPick)
                {
                    line += options[pick].spell == PSpell->getName() ? "; cast the pick" : fmt::format("; cast {}", PSpell->getName());
                }
                else if (missing <= 0)
                {
                    line += fmt::format("; cast {} on a full target", PSpell->getName());
                }
                return line;
            }
            if (auto* PMob = asMob(PTarget); PMob != nullptr && r != nullptr)
            {
                if (const auto* p = priced(PSpell->getID()); p != nullptr)
                {
                    return priceDebuff(*r, spotAverages(r->zone, r->mobName), x, PCaster, PMob, PSpell, *p, true, nullptr).line(fmt::format("bank: {} casts ", PCaster->getName()));
                }
            }
            if (r == nullptr)
            {
                return ""; // a buff between fights: nothing to price yet
            }
            return unpricedLine(PCaster->getName(), PSpell->getName(), PTarget != nullptr ? PTarget->getName() : "nobody", familyOf(PSpell));
        }

        auto onAlready(CSpell* PSpell, CBattleEntity* PTarget) -> std::optional<double>
        {
            const auto* p = PSpell != nullptr ? priced(PSpell->getID()) : nullptr;
            if (p == nullptr || PTarget == nullptr)
            {
                return std::nullopt;
            }
            const auto* PEffect = PTarget->StatusEffectContainer->GetStatusEffect(p->effect);
            // A higher tier overwrites what is on; a tier the effect does not
            // carry is taken as on already
            if (PEffect == nullptr || (PEffect->GetTier() > 0 && p->tier > PEffect->GetTier()))
            {
                return std::nullopt;
            }
            return secondsLeft(PEffect);
        }

        auto blockedOn(CSpell* PSpell, CBattleEntity* PTarget) -> bool
        {
            const auto* p = PSpell != nullptr ? priced(PSpell->getID()) : nullptr;
            if (p == nullptr || PTarget == nullptr || PTarget->StatusEffectContainer->GetStatusEffect(p->effect) != nullptr)
            {
                return false;
            }
            sol::protected_function nullified = ::lua["xi"]["data"]["statusEffect"]["isEffectNullified"];
            auto                    res       = nullified(CLuaBaseEntity(PTarget), static_cast<uint16>(p->effect), p->tier);
            return !failed("isEffectNullified", res) && res.get<bool>(0);
        }

        auto castRange(CBattleEntity* PCaster, CSpell* PSpell, CBattleEntity* PTarget) -> float
        {
            if (PCaster == nullptr || PSpell == nullptr || PTarget == nullptr)
            {
                return 0.0f;
            }
            float reach = PSpell->getRange() + PCaster->modelHitboxSize + PTarget->modelHitboxSize;
            const auto family = PSpell->getSpellFamily();
            if (PCaster->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Entrust) &&
                (family == SPELLFAMILY_INDI_BUFF || family == SPELLFAMILY_INDI_DEBUFF))
            {
                reach = 25.0f;
            }
            return std::min(reach, 40.0f); // magic state's outer range limit
        }

        auto usable(CBattleEntity* PCaster, const SpellID id) -> bool
        {
            CSpell* PSpell = spell::GetSpell(id);
            if (!CSpellBook::Eligible(PCaster, PSpell))
            {
                return false;
            }
            if (auto* PChar = dynamic_cast<CCharEntity*>(PCaster); PChar != nullptr && PChar->PRecastContainer->Has(RECAST_MAGIC, static_cast<Recast>(id)))
            {
                return false;
            }
            return battleutils::CalculateSpellCost(PCaster, PSpell) <= PCaster->health.mp;
        }

        auto expectedCure(CBattleEntity* PCaster, CSpell* PSpell, CBattleEntity* PTarget) -> std::optional<int32>
        {
            if (PCaster == nullptr || PSpell == nullptr)
            {
                return std::nullopt;
            }
            auto fn = bankFunction("expectedCure");
            if (!fn.has_value())
            {
                return std::nullopt;
            }
            auto res = PTarget != nullptr ? (*fn)(CLuaBaseEntity(PCaster), static_cast<uint16>(PSpell->getID()), static_cast<uint8>(PSpell->getElement()), CLuaBaseEntity(PTarget))
                                          : (*fn)(CLuaBaseEntity(PCaster), static_cast<uint16>(PSpell->getID()), static_cast<uint8>(PSpell->getElement()), sol::nil);
            if (failed("expectedCure", res) || res.get_type(0) != sol::type::number)
            {
                return std::nullopt;
            }
            return static_cast<int32>(res.get<double>(0));
        }

        // The formula's number when it answers, known; the learned estimate
        // when it does not (Rapture up, or the sampler failed), a floor
        // until an uncapped cure has taught it
        auto cureTiers(CBattleEntity* PCaster, const CureAvailability availability) -> std::vector<CureTier>
        {
            std::vector<CureTier> out;
            for (const auto tier : cardian::tactician::kCureTiers)
            {
                const auto id = static_cast<SpellID>(tier);
                CSpell* PTier = spell::GetSpell(id);
                if (!(availability == CureAvailability::Ready ? usable(PCaster, id) : CSpellBook::Eligible(PCaster, PTier)))
                {
                    continue;
                }
                // The formula's number, known; else the learned floor, and
                // never "known" from a cast that landed under other buffs
                auto [heals, known] = cureEstimate(PCaster->id, PTier);
                if (const auto computed = expectedCure(PCaster, PTier); computed.has_value())
                {
                    heals = *computed;
                    known = true;
                }
                else
                {
                    known = false;
                }
                out.push_back(CureTier{ id, CureOption{ .spell = PTier->getName(), .mp = battleutils::CalculateSpellCost(PCaster, PTier), .heals = heals, .known = known } });
            }
            return out;
        }

        auto pickTier(const std::vector<CureTier>& tiers, CBattleEntity* PTarget, const bool requested) -> SpellID
        {
            if (PTarget == nullptr || PTarget->isDead())
            {
                return static_cast<SpellID>(0);
            }
            std::vector<CureOption> options;
            for (const auto& tier : tiers)
            {
                options.push_back(tier.option);
            }
            const auto pick = pickCure(options, PTarget->GetMaxHP() - PTarget->health.hp, requested);
            return pick == kNoPick ? static_cast<SpellID>(0) : tiers[pick].id;
        }

        auto melee(FightRecord& r, CBattleEntity* PActor, CBattleEntity* PTarget, const bool allowSample) -> std::optional<FightRecord::MeleeGuess>
        {
            if (PActor == nullptr || PTarget == nullptr)
            {
                return std::nullopt;
            }
            const auto key = std::make_pair(PActor->id, PTarget->id);
            if (const auto it = r.meleeCache.find(key); it != r.meleeCache.end())
            {
                return it->second;
            }
            // No main weapon, no swings: the game builds none either
            auto* PMain = dynamic_cast<CItemWeapon*>(PActor->m_Weapons[SLOT_MAIN]);
            if (PMain == nullptr)
            {
                r.meleeCache[key] = std::nullopt;
                return std::nullopt;
            }
            const auto type = PMain->getSkillType();
            const bool h2h  = type == xi::SkillType::HandToHand;
            auto       fn   = bankFunction("swingBase");
            if (!fn.has_value())
            {
                return std::nullopt;
            }
            auto res = (*fn)(CLuaBaseEntity(PActor), CLuaBaseEntity(PTarget), h2h);
            if (failed("swingBase", res) || res.get_type(0) != sol::type::number)
            {
                r.meleeCache[key] = std::nullopt; // declined (Consume Mana up): not asked again this fight
                return std::nullopt;
            }
            const double base   = res.get<double>(0);
            const auto   sample = pdifOf(PActor, PTarget->GetMLevel(), PTarget->DEF(), type, allowSample);
            if (!sample.has_value())
            {
                return std::nullopt; // over budget this call: asked again next time
            }
            // Swings a round as the game builds them: the weapon's own hit
            // count (two bare-handed unless the mob swings once, a mob's
            // multi-hit as its mean), the offhand when dual wielding, then
            // the quadruple, triple and double attack rolls taken in that
            // order, one of them at most, and a monk's kicks on top
            auto*  PMob   = asMob(PActor);
            double swings = h2h ? (PMob != nullptr && PMob->getMobMod(xi::MobMod::H2hSingleSwing) > 0 ? 1.0 : 2.0) : std::max<double>(1.0, PMain->getHitCount());
            if (PMob != nullptr)
            {
                if (const auto multi = PMob->getMobMod(xi::MobMod::MultiHit); multi > 0)
                {
                    swings = 1.0 + (1.0 + multi) / 2.0;
                }
            }
            if (PActor->IsDualWielding())
            {
                swings += 1.0;
            }
            const double qa    = std::clamp(PActor->getMod(xi::Mod::QUAD_ATTACK) / 100.0, 0.0, 1.0);
            const double ta    = std::clamp(PActor->getMod(xi::Mod::TRIPLE_ATTACK) / 100.0, 0.0, 1.0);
            const double da    = std::clamp(PActor->getMod(xi::Mod::DOUBLE_ATTACK) / 100.0, 0.0, 1.0);
            const double extra = qa * 3.0 + (1.0 - qa) * (ta * 2.0 + (1.0 - ta) * da);
            swings += extra;
            if (h2h)
            {
                swings += std::clamp(PActor->getMod(xi::Mod::KICK_ATTACK_RATE) / 100.0, 0.0, 1.0);
            }
            const double hitRate = battleutils::GetHitRate(PActor, PTarget) / 100.0;
            const double delay   = PActor->GetWeaponDelay(false) / 1000.0;

            FightRecord::MeleeGuess g;
            g.perRound        = swings * hitRate * base * sample->mean;
            g.perSecond       = delay > 0.0 ? g.perRound / delay : 0.0;
            g.biggest         = base * sample->biggest; // a critical swing at the biggest ratio sampled
            r.meleeCache[key] = g;
            return g;
        }

        auto pricesFor(FightRecord& r, const SpotAverages& spot, const Exchange& x, const std::vector<CBattleEntity*>& members, CBattleEntity* PMember, CMobEntity* PMob,
                       const std::function<bool(SpellID)>& admitted) -> std::vector<DebuffPrice>
        {
            std::vector<DebuffPrice> out;
            for (const auto& p : kPriced)
            {
                CSpell* PSpell = spell::GetSpell(p.id);
                if (!CSpellBook::Eligible(PMember, PSpell) || (admitted && !admitted(p.id)))
                {
                    continue;
                }
                out.push_back(priceDebuff(r, spot, x, PMember, PMob, PSpell, p, false, &members));
            }
            return out;
        }

        auto priceMember(FightRecord& r, const SpotAverages& spot, const Exchange& x, const std::vector<CBattleEntity*>& members, CBattleEntity* PMember, CMobEntity* PMob) -> std::vector<std::string>
        {
            std::vector<std::string> out;
            for (const auto& p : pricesFor(r, spot, x, members, PMember, PMob))
            {
                out.push_back(p.line(fmt::format("bank: {} could cast ", PMember->getName())));
            }
            return out;
        }

        void defenceSplit(FightRecord& r, CBattleEntity* PMember, CMobEntity* PMob, const int32 landed)
        {
            auto* PChar = PMember != nullptr && PMember->objtype == TYPE_PC ? static_cast<CCharEntity*>(PMember) : nullptr;
            if (PChar == nullptr || landed <= 0)
            {
                return;
            }
            if (debug() && selfChecked.insert(PChar->id).second)
            {
                if (const auto ratio = selfCheck(PChar, PMob, PMob->DEF()); ratio)
                {
                    ShowInfoFmt("tactics: bank: self-check {} on {}: {:.2f} ({} rolls a side at defence {})", PChar->getName(), PMob->getName(), *ratio, kPdifRolls, PMob->DEF());
                }
            }
            auto d = defenceOf(PMob);
            if (d.down == 0)
            {
                return;
            }
            const auto eNow  = pdif(PChar, PMob, d.now);
            const auto eBase = pdif(PChar, PMob, d.base());
            if (!eNow || !eBase)
            {
                return;
            }
            const double without = withoutDefenceDown(landed, *eNow, *eBase);
            const double extra   = landed - without;
            r.defDownExtra += extra;
            ++r.defDownHits;
            apportion(extra, d.effects);
            for (const auto& share : d.effects)
            {
                r.creditFor(share.effect).hp += share.hp;
            }
            if (debug())
            {
                std::string effects;
                for (const auto& e : d.effects)
                {
                    effects += fmt::format("{}{} {}%", effects.empty() ? "" : ", ", e.effect, e.defp);
                }
                ShowInfoFmt("tactics: bank: {}'s {} on {} would be {:.0f} at base defence {} (now {}: {})", PChar->getName(), landed, PMob->getName(), without, d.base(), d.now, effects);
            }
        }

        void dotSplit(FightRecord& r, CMobEntity* PMob, const int32 landed)
        {
            if (PMob == nullptr || landed <= 0)
            {
                return;
            }
            // Each effect's share of the regen the mob is losing: most carry
            // the modifier on the effect; Poison, Requiem and Kaustra put it
            // on the entity, so their share is their power
            std::vector<std::pair<std::string, int32>> shares;
            for (const auto& [effect, tier, amount] : effectsWith(PMob, xi::Mod::REGEN_DOWN))
            {
                if (amount > 0)
                {
                    shares.emplace_back(nameOf(effect), amount);
                }
            }
            for (const auto effect : { xi::StatusEffect::Poison, xi::StatusEffect::Requiem, xi::StatusEffect::Kaustra })
            {
                if (const auto* PEffect = PMob->StatusEffectContainer->GetStatusEffect(effect); PEffect != nullptr && PEffect->GetPower() > 0)
                {
                    shares.emplace_back(nameOf(effect), PEffect->GetPower());
                }
            }
            std::vector<std::pair<std::string, double>> split;
            apportionTicks(landed, shares, split);
            for (const auto& [effect, hp] : split)
            {
                r.creditFor(effect).ticks += hp;
                r.dotDealt += hp;
            }
        }
    } // namespace bank
} // namespace pawn::tactics
