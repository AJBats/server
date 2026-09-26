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
#include "spell_bank.h"
#include "tactics.h"

#include "common/logging.h"

#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "ipc_client.h"
#include "party.h"
#include "spell.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pawn::tactics::role
{
    // What the open fights threaten one member with: the biggest hit
    // any has landed on anyone (a switch can bring it to her), the
    // spot's memory of it, or the formulas' guess before either; and
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
            t.biggestHit = std::max({ t.biggestHit, top != nullptr ? static_cast<double>(top->biggestHit) : 0.0, spot.biggestHit.mean, guess ? guess->biggest : 0.0 });

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
    } // namespace

    void sayParty(CCharEntity* PChar, const std::string& text)
    {
        if (PChar->PParty == nullptr)
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
            for (const auto& p : bank::pricesFor(r, spotAverages(r.zone, r.mobName), log.exchange(), scope.members, PHolder, PMob, admitted))
            {
                const auto by = admittedBy(PHolder, static_cast<SpellID>(p.id), PMob);
                if (!p.go() || !by.has_value() || !bank::usable(PHolder, static_cast<SpellID>(p.id)))
                {
                    continue;
                }
                conveyor.feed(Conveyor::keyFor(spell::GetSpell(static_cast<SpellID>(p.id)), PMob->id, PHolder->id),
                              Request{ .source     = Source::Role,
                                       .caster     = PHolder->id,
                                       .rowId      = *by,
                                       .spell      = p.id,
                                       .fedAt      = now,
                                       .score      = p.noData ? 0.0 : p.mp - p.mpWorth,
                                       .landChance = p.landChance,
                                       .why        = fmt::format("worth {:.0f} MP against {}", p.mpWorth, p.mp) },
                              scope);
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
