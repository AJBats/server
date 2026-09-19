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
    namespace
    {
        auto fighting(const FightLog& log) -> bool
        {
            return std::any_of(log.open().begin(), log.open().end(), [](const FightRecord& r)
                               {
                                   return !r.settling();
                               });
        }

    } // namespace

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
        const bool inFight = fighting(log);

        // Her line: the missing HP a cure is worth casting at -- the biggest
        // tier she has in a fight, the smallest between. What she has, not
        // what she can afford this moment: a need she cannot serve is still
        // a need another mage can. A known tier waits for a quarter more
        // than it heals, so no cure is ever capped (the user's rule,
        // 2026-09-15); a tier known only by its floor waits for twice that,
        // so the first cure lands whole and teaches the number
        int32 line = 0;
        for (const auto& tier : bank::cureTiers(PHolder, bank::CureAvailability::Eligible))
        {
            const int32 heals = tier.option.known ? static_cast<int32>(std::lround(tier.option.heals * 1.25)) : 2 * tier.option.heals;
            line              = line == 0 ? heals : (inFight ? std::max(line, heals) : std::min(line, heals));
        }
        if (line > 0)
        {
            for (auto* PMember : scope.members)
            {
                if (!curable(PHolder, PMember))
                {
                    continue;
                }
                const int32 missing = PMember->GetMaxHP() - PMember->health.hp;
                if (!cardian::tactics::cureWanted(missing, line))
                {
                    continue;
                }
                conveyor.feed({ NeedKind::Cure, 0, PMember->id },
                              Request{ .source = Source::Role, .caster = PHolder->id, .fedAt = now, .score = -static_cast<double>(missing), .why = fmt::format("{} missing", missing) },
                              scope);
            }
        }

        // The debuffs worth their MP on the mobs the party is on, once she
        // is in the fight (a first cast on a mob nobody has struck is a pull)
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
            for (const auto& p : bank::pricesFor(r, spotAverages(r.zone, r.mobName), log.exchange(), scope.members, PHolder, PMob))
            {
                if (!p.go() || !bank::usable(PHolder, static_cast<SpellID>(p.id)))
                {
                    continue;
                }
                conveyor.feed(Conveyor::keyFor(spell::GetSpell(static_cast<SpellID>(p.id)), PMob->id, PHolder->id),
                              Request{ .source     = Source::Role,
                                       .caster     = PHolder->id,
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
