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
#include "pawn.h"
#include "pawn_spellbook.h"
#include "spell_bank.h"
#include "tactician_line.h"
#include "tactics.h"

#include "common/logging.h"

#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "ipc_client.h"
#include "party.h"
#include "spell.h"
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
    // the rate she takes -- her own this fight, else the spot's or the
    // guess only when the mob is on her, else nothing
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

            const double secs = r.seconds(now);
            const auto*  m    = r.find(PMember->id);
            const double own  = m != nullptr && secs > 0.0 ? m->damageTaken / secs : 0.0;
            if (own > 0.0)
            {
                t.takenPerSecond += own;
            }
            else if (r.hitting == PMember->id)
            {
                t.takenPerSecond += spot.fights > 0 ? spot.takenPerSecond.mean : (guess ? guess->perSecond : 0.0);
            }
        }
        return t;
    }

    namespace
    {
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

    void think(CCharEntity* PHolder, FightLog& log, Conveyor& conveyor, const Conveyor::Scope& scope, const bool engaged, const double now)
    {
        // Her line, per member: the missing HP a cure is worth casting at --
        // where her smallest tier lands whole (bank_math.h wholeAt), in a
        // fight and between (the user, 2026-09-25: a healer tops up), among
        // the tiers her allow-list lets her cast on that member
        // (tactician_line.h). What she has, not what she can afford this
        // moment: a need she cannot serve is still a need another mage can.
        // Which tier she casts is the conveyor's (pickWhole)
        const auto tiers = bank::cureTiers(PHolder, bank::CureAvailability::Eligible);
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
            const int32 missing = PMember->GetMaxHP() - PMember->health.hp;
            if (line == 0 || !cardian::tactics::cureWanted(missing, line))
            {
                continue;
            }
            conveyor.feed({ NeedKind::Cure, 0, PMember->id },
                          Request{ .source = Source::Role, .caster = PHolder->id, .rowId = row, .fedAt = now, .score = -static_cast<double>(missing), .why = fmt::format("{} missing", missing) },
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

    void cycleOpened(CCharEntity* PHolder, Pace& pace)
    {
        pace.opened(PHolder->health.mp);
    }

    void cycleClosed(CCharEntity* PHolder, const int32 spent, Pace& pace)
    {
        pace.closed(PHolder->health.mp, spent);
    }

    void speakPace(CCharEntity* PHolder, Pace& pace)
    {
        if (!pace.measured() || pace.behind() == pace.saidBehind)
        {
            return;
        }
        pace.saidBehind = pace.behind();
        ShowInfoFmt("tactics: pace: {}", pace.line(PHolder->getName()));
        sayParty(PHolder, pace.behind() ? fmt::format("Behind pace: a fight here costs me ~{:.0f} MP and I net {:+.0f} between fights.", pace.spent.mean, pace.regained.mean)
                                        : std::string("Back on pace."));
    }
} // namespace pawn::tactics::role
