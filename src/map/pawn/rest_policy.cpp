// Cardian: Support Mage resting, shared party wake-up decisions (slice 5).
#include "rest_policy.h"

#include "fight_log.h"
#include "pawn_controller.h"
#include "rest_math.h"
#include "role_support.h"
#include "spell_bank.h"

#include "ai/ai_container.h"
#include "common/logging.h"
#include "common/settings.h"
#include "entities/char_entity.h"
#include "recast_container.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace pawn::tactics
{
    namespace
    {
        auto controller(CBattleEntity* p) -> CPawnController*
        {
            return p != nullptr && p->PAI != nullptr ? dynamic_cast<CPawnController*>(p->PAI->GetController()) : nullptr;
        }

    }

    void RestPlanner::reset(const uint32 closedCount)
    {
        m_since = closedCount;
        m_advice.clear();
        m_recovery.clear();
    }

    void RestPlanner::observe(CBattleEntity* member, const double now)
    {
        if (member == nullptr)
        {
            return;
        }
        const auto it = m_recovery.find(member->id);
        if (it == m_recovery.end())
        {
            return;
        }
        auto& memory = it->second;
        const auto zone = static_cast<uint16>(member->getZone());
        if (memory.body != member || memory.zone != zone)
        {
            memory = {};
            memory.body = member;
            memory.zone = zone;
        }
        memory.flow.observe(now, member->health.mp, member->GetMaxMP());
        if (const auto advice = m_advice.find(member->id); advice != m_advice.end())
        {
            auto& a = advice->second;
            const auto rates = memory.flow.rates(now);
            const double firstRecovery = 2.0 * settings::get<uint8>("map.HEALING_TICK_DELAY");
            const auto pace = cardian::rest::pacing(member->health.mp, member->GetMaxMP(), rates,
                a.readyMp, a.knownCost, member->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing),
                firstRecovery, cardian::rest::mpAtTick(2, member->getMod(xi::Mod::CLEAR_MIND), member->getMod(xi::Mod::MPHEAL)), a.cheapestCureMp, a.fightSpentMp);
            a.recover = pace.recover;
            a.criticalMp = pace.critical;
            a.spentPerSecond = rates.spent;
            a.recoveredPerSecond = rates.recovered;
            a.reserveMp = pace.reserve;
            a.projectedMp = pace.projected;
            a.recoveryTargetMp = pace.target;
        }
    }

    auto RestPlanner::advice(const uint32 id) const -> std::optional<RestAdvice>
    {
        const auto it = m_advice.find(id);
        return it == m_advice.end() ? std::nullopt : std::optional<RestAdvice>{it->second};
    }

    void RestPlanner::tick(FightLog& log, const Conveyor::Scope& scope, const double now, std::span<const cardian::cure::Choice> emergency)
    {
        m_advice.clear();
        const auto count = std::min<std::size_t>(log.closedCount() - std::min(m_since, log.closedCount()), log.recent().size());
        for (auto* member : scope.members)
        {
            auto* control = controller(member);
            if (control == nullptr || member->isDead() || !scope.holders.contains(member->id) || !control->Gambits().MasterOn())
            {
                continue;
            }
            auto* body = static_cast<CCharEntity*>(member);
            auto& a = m_advice[body->id];
            std::vector<double> costs;
            for (std::size_t i = 0; i < count && costs.size() < 8; ++i)
            {
                const auto& fight = log.recent()[i];
                if (fight.zone == static_cast<uint16>(body->getZone()))
                {
                    if (const auto* figures = fight.find(body->id); figures != nullptr)
                    {
                        costs.push_back(figures->mpSpent);
                    }
                }
            }
            a.knownCost = !costs.empty();
            a.readyMp = body->GetMaxMP(); // readiness display until history exists; pacing has its own startup reserve
            if (a.knownCost)
            {
                a.readyMp = cardian::rest::fightBudget(std::move(costs));
            }
            for (const auto& fight : log.open())
            {
                if (!fight.settling() && fight.zone == static_cast<uint16>(body->getZone()))
                {
                    if (const auto* figures = fight.find(body->id); figures != nullptr)
                    {
                        a.fightSpentMp += figures->mpSpent;
                    }
                }
            }
            auto tiers = bank::cureTiers(body, bank::CureAvailability::Eligible);
            double cheapestCure = 0.0;
            for (const auto& tier : tiers)
            {
                if (tier.option.mp > 0)
                {
                    cheapestCure = cheapestCure == 0.0 ? tier.option.mp : std::min(cheapestCure, static_cast<double>(tier.option.mp));
                }
            }
            a.cheapestCureMp = cheapestCure;
            m_recovery.try_emplace(body->id);
            // Event callbacks and body decisions also sample this ledger.
            // Use their live clock, not the earlier shared frame timestamp.
            observe(body, seconds(timer::now()));
        }

        // Measurement and emergency selection already ran this party tick.
        // Ordinary upcoming work does not veto MP pacing (RESEARCH §12.8).
        for (const auto& choice : emergency)
        {
            if (auto it = m_advice.find(choice.cure.caster); it != m_advice.end())
            {
                it->second.wake = true;
                it->second.why = fmt::format("emergency readiness: Cure starts in {:.1f}s, lands in {:.1f}s for ~{:.0f} HP",
                    choice.cure.time.start, choice.cure.time.land, choice.cure.heals);
            }
        }
        std::erase_if(m_recovery, [&](const auto& entry) { return !m_advice.contains(entry.first); });
        if (debug())
        {
            for (auto* member : scope.members)
            {
                const auto it = m_advice.find(member->id);
                if (it == m_advice.end())
                {
                    continue;
                }
                const auto& a = it->second;
                auto& memory = m_recovery.at(member->id);
                if (now >= memory.logAt || a.recover != memory.recovering)
                {
                    memory.logAt = now + 10.0;
                    memory.recovering = a.recover;
                    ShowInfoFmt("rest: pace {}: {}{}; MP {}/{}, spent {:.1f}/min recovered {:.1f}/min; projected {:.1f}, reserve {:.1f}, recovery target {:.1f}; fight budget {:.1f}, spent this fight {:.1f}",
                        member->getName(), a.recover ? "recovery due" : "stand ready", a.criticalMp ? " (critical MP)" : "",
                        member->health.mp, member->GetMaxMP(), 60.0 * a.spentPerSecond, 60.0 * a.recoveredPerSecond,
                        a.projectedMp, a.reserveMp, a.recoveryTargetMp, a.readyMp, a.fightSpentMp);
                }
            }
        }
    }

    auto RestPlanner::lines(const Conveyor::Scope& scope) const -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (auto* member : scope.members)
        {
            if (const auto a = advice(member->id); a.has_value())
            {
                out.push_back(fmt::format("rest: {}: {}{}; ready at {:.0f} MP{}", member->getName(),
                    a->wake ? "stand: " : a->recover ? "recover when free" : "stand ready", a->why, a->readyMp,
                    a->knownCost ? " (fight + link reserve)" : " (no fight cost yet)"));
                out.push_back(fmt::format("rest: {}: spent {:.1f}/min, recovered {:.1f}/min; projected {:.0f} MP, reserve {:.0f}, recovery target {:.0f}",
                    member->getName(), 60.0 * a->spentPerSecond, 60.0 * a->recoveredPerSecond,
                    a->projectedMp, a->reserveMp, a->recoveryTargetMp));
            }
        }
        return out;
    }
}
