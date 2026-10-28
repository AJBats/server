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

#include "role_support.h"

#include "conveyor.h"
#include "fight_log.h"
#include "party_roster.h"
#include "pawn.h"
#include "pawn_spellbook.h"
#include "regen_math.h"
#include "spell_bank.h"
#include "tactician_line.h"
#include "tactics.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"

#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "ipc_client.h"
#include "party.h"
#include "spell.h"
#include "status_effect.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pawn::tactics::role
{
    // What the open fights threaten one member with: the biggest hit
    // any has landed on anyone (a switch can bring it to her), the
    // worst the spot remembers -- the risk of death, not the likely
    // hit -- or the formulas' guess before either; and
    // the rate she takes -- her own this fight, blended with the spot's or
    // the guess while the mob is on her (below)
    auto threat(FightLog& log, CBattleEntity* PMember, const double now) -> Threat
    {
        Threat t;
        for (auto& r : log.open())
        {
            if (r.settling())
            {
                continue;
            }
            const auto& spot     = spotAverages(r.zone, r.mobName);
            const auto* top      = r.biggest();
            const bool  onRecord = top != nullptr || spot.fights > 0;
            std::optional<FightRecord::MeleeGuess> guess;
            auto* PMob = dynamic_cast<CMobEntity*>(zoneutils::GetEntity(r.mobId, TYPE_MOB));
            if (!onRecord && PMob != nullptr && PMob->id == r.mobId)
            {
                guess = bank::melee(r, PMob, PMember, true);
            }
            t.biggestHit = std::max({ t.biggestHit, top != nullptr ? static_cast<double>(top->biggestHit) : 0.0, spot.worstHit, guess ? guess->biggest : 0.0 });

            // The rate she takes from this fight: what she has taken, with the
            // rate expected of the mob on her counted as
            // pawn.TACTICS_RATE_PRIOR_SECONDS of evidence ahead of it -- the
            // spot's record, else the formulas' guess, while the mob is on
            // her, weighed over the seconds it has been on her (a pull's run
            // in, or the time it spent on the tank, is no evidence about her).
            // A fight's first hit reads as a hit, not a torrent, and a mob
            // known to hit hard reads as hard from its first second; as the
            // fight runs, what she has taken has the say. With nothing
            // expected of it, what she has taken over at least that long
            static const double prior    = std::max(0.5, static_cast<double>(settings::get<float>("pawn.TACTICS_RATE_PRIOR_SECONDS")));
            const double        secs     = std::max(0.0, r.seconds(now));
            const auto*         m        = r.find(PMember->id);
            const double        taken    = m != nullptr ? static_cast<double>(m->damageTaken) : 0.0;
            double              expected = 0.0;
            if (r.hitting == PMember->id)
            {
                if (spot.fights > 0)
                {
                    expected = spot.takenPerSecond.mean;
                }
                else
                {
                    if (!guess.has_value() && PMob != nullptr && PMob->id == r.mobId)
                    {
                        guess = bank::melee(r, PMob, PMember, true);
                    }
                    expected = guess.has_value() ? guess->perSecond : 0.0;
                }
            }
            if (expected > 0.0)
            {
                const double onHer = r.hittingSince > 0.0 ? std::clamp(now - r.hittingSince, 0.0, secs) : secs;
                t.takenPerSecond += (taken + expected * prior) / (onHer + prior);
                t.expectedPerSecond += expected;
            }
            else if (taken > 0.0)
            {
                t.takenPerSecond += taken / std::max(secs, prior);
            }
        }
        return t;
    }

    namespace
    {
        auto regenOn(const CBattleEntity* PMember) -> CStatusEffect*
        {
            return PMember != nullptr && PMember->StatusEffectContainer != nullptr ? PMember->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Regen) : nullptr;
        }

        auto secondsOf(const timer::duration d) -> double
        {
            return std::chrono::duration<double>(d).count();
        }

        auto curable(const CCharEntity* PHolder, CBattleEntity* PMember) -> bool
        {
            return PMember != nullptr && PMember->objtype == TYPE_PC && !PMember->isDead() && PMember->loc.zone == PHolder->loc.zone;
        }

        // Dia and Diaga, or Bio: the two sides of the party's Dia or Bio
        // order, which block each other on a mob
        auto isDiaSide(const uint32 family) -> bool
        {
            return family == SPELLFAMILY_DIA || family == SPELLFAMILY_DIAGA;
        }

        // The party's Dia or Bio order (pawn.h HuntRules::preferBio; the
        // user, 2026-10-03): her tactician casts the one preferred while
        // anyone it can ask is able to -- a member in the zone and alive,
        // with a marked row admitting it on the mob, who has learned it and
        // whose jobs and level allow it (CSpellBook::Eligible; her MP or its
        // recast only a pause) -- and the other once nobody is. The
        // preferred one up on the mob already, whoever cast it, is the order
        // kept: the other is not cast over it (Bio I would wipe a Dia I).
        // Plain rows ignore the order and race through the conveyor's one
        // Dia-or-Bio need
        auto setAsideByOrder(CCharEntity* PHolder, const uint16 spell, CMobEntity* PMob, const Conveyor::Scope& scope) -> bool
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(spell));
            if (PSpell == nullptr)
            {
                return false;
            }
            const auto family = static_cast<uint32>(PSpell->getSpellFamily());
            const bool dia    = isDiaSide(family);
            const bool bio    = family == SPELLFAMILY_BIO;
            if (!dia && !bio)
            {
                return false;
            }
            const bool preferBio = pawn::huntRulesOf(pawn::ordersOwnerOf(PHolder)).preferBio;
            if (bio == preferBio)
            {
                return false;
            }
            if (PMob->StatusEffectContainer->HasStatusEffect(preferBio ? xi::StatusEffect::Bio : xi::StatusEffect::Dia))
            {
                return true;
            }
            for (auto* PMember : scope.members)
            {
                if (PMember == nullptr || PMember->isDead() || PMember->loc.zone != PMob->loc.zone)
                {
                    continue;
                }
                for (const auto& debuff : cardian::tactician::kPricedDebuffs)
                {
                    auto* PWanted = spell::GetSpell(static_cast<SpellID>(debuff.id));
                    if (PWanted == nullptr)
                    {
                        continue;
                    }
                    const auto wantedFamily = static_cast<uint32>(PWanted->getSpellFamily());
                    const bool wanted       = preferBio ? wantedFamily == SPELLFAMILY_BIO : isDiaSide(wantedFamily);
                    if (wanted && CSpellBook::Eligible(PMember, PWanted) && admittedBy(PMember, static_cast<SpellID>(debuff.id), PMob).has_value())
                    {
                        if (debug())
                        {
                            ShowInfoFmt("tactics: {} sets {} aside on {}: the party prefers {}, and {} can cast {}", PHolder->getName(), PSpell->getName(), PMob->getName(),
                                        preferBio ? "Bio" : "Dia", PMember->getName(), PWanted->getName());
                        }
                        return true;
                    }
                }
            }
            return false;
        }
    } // namespace

    auto regenLeft(const CBattleEntity* PMember) -> int32
    {
        const auto* PEffect = regenOn(PMember);
        if (PEffect == nullptr)
        {
            return 0;
        }
        const double left = secondsOf(PEffect->GetDuration()) - secondsOf(timer::now() - PEffect->GetStartTime());
        return static_cast<int32>(cardian::regen::healsLeft(PEffect->GetPower(), left));
    }

    void sayParty(CCharEntity* PChar, const std::string& text)
    {
        // A played character's party chat is his own: the tactician never
        // speaks in his name
        if (PChar->PParty == nullptr || PChar->PSession != nullptr)
        {
            return;
        }
        message::send(ipc::ChatMessageParty{
            .partyId    = PChar->PParty->GetPartyID(),
            .senderId   = PChar->id,
            .senderName = PChar->getName(),
            .message    = text,
            .zoneId     = PChar->getZone(),
            .gmLevel    = PChar->m_GMlevel,
        });
    }

    void sayPace(CCharEntity* PChar, const std::string& text)
    {
        if (pawn::roster::roleOf(PChar) == cardian::party::Role::Healer)
        {
            sayParty(PChar, text);
        }
    }

    void think(CCharEntity* PHolder, FightLog& log, Conveyor& conveyor, const Conveyor::Scope& scope, const bool engaged, const double now)
    {
        // Her line, per member: the missing HP a cure is worth casting at --
        // where her smallest tier lands whole (bank_math.h wholeAt), in a
        // fight and between (the user, 2026-09-25: a healer tops up), among
        // the tiers her allow-list lets her cast on that member
        // (tactician_line.h). What she has, not what she can afford this
        // moment: a need she cannot serve is still a need another mage can.
        // Which tier she casts is the conveyor's (pickWhole)
        // A Black Mage cures as routine only from the Healer seat (the user,
        // 2026-10-17), and any mage in the Damage seat cures as a Black Mage
        // does (the user, 2026-10-27: a Red Mage dealing damage leaves the
        // top-ups to the healers): she joins the cures for first aid alone,
        // which the party's tick chooses apart (chooseFirstAid). With no seat
        // taken she cures as any mage does
        const auto seat         = pawn::roster::roleOf(PHolder);
        const bool routineCures = seat == cardian::party::Role::Healer || seat == cardian::party::Role::None ||
                                  (PHolder->GetMJob() != xi::Job::BLM && seat != cardian::party::Role::Damage);
        auto       tiers        = bank::cureTiers(PHolder, bank::CureAvailability::Eligible);
        if (!routineCures)
        {
            tiers.clear();
        }

        // A fight on, routine cures keep a buffer (conveyor_math.h
        // cureBuffer), and a Regen is judged on the time the fight has left
        bool fightOn = false;
        for (const auto& r : log.open())
        {
            fightOn = fightOn || !r.settling();
        }
        const double fightLeft = fightOn ? std::max(0.0, pawn::tactics::fightLeft(PHolder)) : 0.0;

        // Her Regen, the top-up she prefers once she has one (regen_math.h;
        // the user, 2026-10-27): the strongest tier she can cast now, put on
        // the one member whose missing HP and the damage expected before the
        // fight ends spend its whole -- the one taking the most, the tank as
        // a rule. What it heals is its own script run as her cast
        // (bank::expectedRegen). That member's routine cure waits past it
        std::optional<uint32>                  regenFor;
        double                                 regenWhole = 0.0;
        uint16                                 regen      = 0;
        std::vector<cardian::regen::Candidate> candidates;
        std::vector<cardian::regen::Seen>      seens;
        std::vector<std::string>               rows;
        std::vector<uint16>                    regenTiers;
        if (routineCures)
        {
            for (auto* PMember : scope.members)
            {
                if (!curable(PHolder, PMember) || regenOn(PMember) != nullptr)
                {
                    continue;
                }
                const double missing = PMember->GetMaxHP() - PMember->health.hp;
                const double rate    = threat(log, PMember, now).takenPerSecond;
                if (missing <= 0.0 && rate <= 0.0)
                {
                    continue; // nothing for any Regen to heal
                }
                // the strongest tier she can cast that her row admits on her
                for (auto it = cardian::tactician::kRegenTiers.rbegin(); it != cardian::tactician::kRegenTiers.rend(); ++it)
                {
                    const auto tier = static_cast<SpellID>(*it);
                    const auto by   = bank::usable(PHolder, tier) ? admittedBy(PHolder, tier, PMember) : std::nullopt;
                    if (!by.has_value())
                    {
                        continue;
                    }
                    const auto heals = bank::expectedRegen(PHolder, spell::GetSpell(tier), PMember);
                    if (heals.has_value() && cardian::regen::worthCasting({ heals->first, heals->second }, missing, rate, fightLeft))
                    {
                        candidates.push_back({ PMember->id, rate, missing });
                        seens.push_back({ heals->first, heals->second });
                        rows.push_back(*by);
                        regenTiers.push_back(*it);
                    }
                    break;
                }
            }
            if (const auto best = cardian::regen::pick(candidates); best.has_value())
            {
                const auto& c = candidates[*best];
                regen         = regenTiers[*best];
                regenFor      = c.id;
                regenWhole    = cardian::regen::healsWhole(seens[*best]);
                conveyor.feed(Conveyor::keyFor(spell::GetSpell(static_cast<SpellID>(regen)), c.id, PHolder->id),
                              Request{ .source = Source::Role,
                                       .caster = PHolder->id,
                                       .rowId  = rows[*best],
                                       .spell  = regen,
                                       .fedAt  = now,
                                       .score  = -c.takenPerSecond,
                                       .why    = fmt::format("regen: taking {:.1f}/s, {:.0f} missing, the fight {:.0f} s from its end, it heals {:.0f} a tick over {:.0f} s, {:.0f} in all",
                                                             c.takenPerSecond, c.missing, fightLeft, seens[*best].power, seens[*best].seconds, regenWhole) },
                              scope);
            }
        }

        for (auto* PMember : scope.members)
        {
            if (!curable(PHolder, PMember))
            {
                continue;
            }
            int32       line = 0;
            std::string row;
            for (const auto& tier : tiers)
            {
                const auto by = admittedBy(PHolder, tier.id, PMember);
                if (!by.has_value())
                {
                    continue;
                }
                if (row.empty())
                {
                    row = *by;
                }
                const int32 whole = cardian::tactics::wholeAt(tier.option);
                line              = line == 0 ? whole : std::min(line, whole);
            }
            // A Regen on her heals the first of it, and so does her
            // healer's own Regen proposed for her; in a fight the rest waits
            // until it reaches the buffer
            const int32 missing = PMember->GetMaxHP() - PMember->health.hp;
            int32       gap     = cardian::regen::gapAfter(missing, regenLeft(PMember));
            if (regenFor == PMember->id)
            {
                gap = cardian::regen::gapAfter(gap, regenWhole);
            }
            const int32 buffer = cardian::tactics::cureBuffer(PMember->GetMaxHP(), fightOn);
            if (line == 0 || !cardian::tactics::cureWanted(gap, line) || gap < buffer)
            {
                continue;
            }
            conveyor.feed({ NeedKind::Cure, 0, PMember->id },
                          Request{ .source = Source::Role,
                                   .caster = PHolder->id,
                                   .rowId  = row,
                                   .fedAt  = now,
                                   .score  = -static_cast<double>(gap),
                                   .buffer = buffer,
                                   .coming = regenFor == PMember->id ? static_cast<int32>(regenWhole) : 0,
                                   .why    = gap == missing ? fmt::format("{} missing", missing) : fmt::format("{} missing, {} past her Regen", missing, gap) },
                          scope);
        }

        // The debuffs worth their MP on the mobs the party is on, once she
        // is in the fight (a first cast on a mob nobody has struck is a pull),
        // among those her allow-list lets her cast on the mob, asked before
        // the pricing spends its samples
        if (!engaged)
        {
            return;
        }
        for (auto& r : log.open())
        {
            if (r.settling())
            {
                continue;
            }
            auto* PMob = dynamic_cast<CMobEntity*>(zoneutils::GetEntity(r.mobId, TYPE_MOB));
            if (PMob == nullptr || PMob->id != r.mobId || PMob->isDead())
            {
                continue;
            }
            const auto admitted = [&](const SpellID id)
            {
                return admittedBy(PHolder, id, PMob).has_value();
            };
            const auto feed = [&](const cardian::tactics::DebuffPrice& p, const std::string& by)
            {
                conveyor.feed(Conveyor::keyFor(spell::GetSpell(static_cast<SpellID>(p.id)), PMob->id, PHolder->id),
                              Request{ .source     = Source::Role,
                                       .caster     = PHolder->id,
                                       .rowId      = by,
                                       .spell      = p.id,
                                       .fedAt      = now,
                                       .score      = p.noData ? 0.0 : p.mp - p.mpWorth,
                                       .landChance = p.landChance,
                                       .why        = fmt::format("worth {:.0f} MP against {}", p.mpWorth, p.mp) },
                              scope);
            };
            // Dia, Diaga and Bio share one need on the mob, where her
            // proposals would replace one another in the price list's order:
            // she proposes the one that is worth the most past its cost
            std::optional<std::pair<cardian::tactics::DebuffPrice, std::string>> diaOrBio;
            for (const auto& p : bank::pricesFor(r, spotAverages(r.zone, r.mobName), log.exchange(), scope.members, PHolder, PMob, admitted))
            {
                const auto by = admittedBy(PHolder, static_cast<SpellID>(p.id), PMob);
                if (!p.go() || !by.has_value() || !bank::usable(PHolder, static_cast<SpellID>(p.id)) || setAsideByOrder(PHolder, p.id, PMob, scope))
                {
                    continue;
                }
                const auto key = Conveyor::keyFor(spell::GetSpell(static_cast<SpellID>(p.id)), PMob->id, PHolder->id);
                if (key.kind == NeedKind::Status && key.arg == cardian::tactics::kDiaOrBio)
                {
                    if (!diaOrBio.has_value() || p.mpWorth - p.mp > diaOrBio->first.mpWorth - diaOrBio->first.mp)
                    {
                        diaOrBio.emplace(p, *by);
                    }
                    continue;
                }
                feed(p, *by);
            }
            if (diaOrBio.has_value())
            {
                feed(diaOrBio->first, diaOrBio->second);
            }
        }
    }

    void cycleClosed(CCharEntity* PHolder, const int32 spent, Pace& pace)
    {
        pace.closed(PHolder->health.mp, spent);
    }

    void speakPace(CCharEntity* PHolder, Pace& pace)
    {
        if (!pace.measured())
        {
            return;
        }
        const bool behind = pace.behind();
        if (behind != pace.saidBehind)
        {
            pace.saidBehind = behind;
            ShowInfoFmt("tactics: pace: {}", pace.line(PHolder->getName()));
        }
        // The party hears it from the Healer seat only (as sayPace), once
        // each way it turns as the party has heard it: a mage who takes the
        // seat behind pace says so, and none says she is back on a pace she
        // was never heard to be behind
        if (behind != pace.toldBehind && pawn::roster::roleOf(PHolder) == cardian::party::Role::Healer)
        {
            pace.toldBehind = behind;
            sayParty(PHolder, behind ? (pace.regained.mean >= 0.5 ? fmt::format("Behind pace: a fight here costs me ~{:.0f} MP and I only get back ~{:.0f}.", pace.spent.mean, pace.regained.mean)
                                                                  : fmt::format("Behind pace: a fight here costs me ~{:.0f} MP and I get nothing back.", pace.spent.mean))
                                     : std::string("Back on pace."));
        }
    }
} // namespace pawn::tactics::role
