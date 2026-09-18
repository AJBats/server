// Cardian: measured Cure options and emergency selection, without engine actions.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace cardian::cure
{
    constexpr double unavailable = std::numeric_limits<double>::infinity();
    constexpr double safetySeconds = 1.0;

    struct Timing
    {
        double start = unavailable;
        double land = unavailable;
    };

    // Recast runs while standing, finishing an action, and walking. Walking
    // starts after the body is free. No prediction of future MP recovery.
    inline auto timing(bool usable, double recast, double stand, double busy,
                       double walk, double cast) -> Timing
    {
        if (!usable)
        {
            return {};
        }
        const double start = std::max(recast, std::max(stand, busy) + walk);
        return {start, start + cast};
    }

    struct Option
    {
        uint32_t caster = 0;
        uint32_t target = 0;
        uint16_t spell = 0;
        double heals = 0.0;
        double mp = 0.0;
        Timing time;
        double wakeCost = 0.0;
        bool inFlight = false;
    };

    struct Target
    {
        uint32_t id = 0;
        double hp = 0.0;
        double maximum = 0.0;
        double biggest = 0.0;
        double damageRate = 0.0;
        bool tpReady = false;
    };

    struct Choice
    {
        Option cure;
        bool cast = true; // a full-HP TP warning asks for readiness only
        double requiredHp = 0.0; // preserve an emergency through its stand/approach
    };

    // Account for healing when it lands, capped at the target's HP capacity.
    // An unfinished Cure contributes nothing before its actual arrival.
    inline auto margin(const Target& target, double horizon, std::span<const Option> incoming) -> double
    {
        std::vector<Option> events;
        for (const auto& cure : incoming)
        {
            if (cure.target == target.id && cure.time.land <= horizon)
            {
                events.push_back(cure);
            }
        }
        std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.time.land < b.time.land; });
        double hp = target.hp;
        double at = 0.0;
        for (const auto& cure : events)
        {
            hp -= target.damageRate * (cure.time.land - at);
            // A cure arriving after predicted death cannot count as rescue.
            if (hp <= 0.0)
            {
                return hp - target.biggest;
            }
            hp = std::min(target.maximum, hp + cure.heals);
            at = cure.time.land;
        }
        return hp - target.damageRate * (horizon - at) - target.biggest;
    }

    inline auto choose(std::span<const Option> measured, std::span<const Target> targets,
                       std::span<const Choice> previous = {}) -> std::vector<Choice>
    {
        std::vector<Choice> out;
        std::vector<Option> incoming;
        for (const auto& cure : measured)
        {
            if (cure.inFlight)
            {
                incoming.push_back(cure);
            }
        }
        std::unordered_set<uint32_t> assigned;
        // The least HP margin gets first claim on a mage. One mage cannot
        // promise simultaneous first aid to multiple party members.
        std::vector<Target> ordered;
        ordered.reserve(targets.size());
        for (const auto& target : targets) ordered.push_back(target);
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
        {
            return a.hp - a.biggest < b.hp - b.biggest;
        });
        for (const auto& target : ordered)
        {
            double requiredHp = 0.0;
            for (const auto& choice : previous)
            {
                if (choice.cast && choice.cure.target == target.id)
                {
                    requiredHp = std::max(requiredHp, choice.requiredHp);
                }
            }
            const auto remaining = [&](double horizon)
            {
                auto expected = target;
                // Getting up shortens the ETA; that alone must not cancel
                // the emergency and send her straight back to rest. Healing
                // (including a timely incoming Cure) can satisfy this need.
                expected.biggest = std::max(expected.biggest, requiredHp - target.damageRate * horizon);
                return margin(expected, horizon, incoming);
            };
            while (true)
            {
                const Option* best = nullptr;
                for (const auto& cure : measured)
                {
                    if (cure.inFlight || cure.target != target.id || assigned.contains(cure.caster) ||
                        !std::isfinite(cure.time.land) || cure.heals <= 0.0)
                    {
                        continue;
                    }
                    // Earliest landing first; at equal time prefer enough
                    // healing, then cheaper MP, then preserve rest recovery.
                    const double need = std::max(1.0, -remaining(cure.time.land + safetySeconds));
                    const auto better = [&]
                    {
                        if (cure.time.land != best->time.land) return cure.time.land < best->time.land;
                        const double useful = std::min(cure.heals, need);
                        const double other = std::min(best->heals, need);
                        if (useful != other) return useful > other;
                        if (cure.mp != best->mp) return cure.mp < best->mp;
                        if (cure.wakeCost != best->wakeCost) return cure.wakeCost < best->wakeCost;
                        return cure.caster < best->caster;
                    };
                    if (best == nullptr || better()) best = &cure;
                }
                if (best == nullptr) break;
                const double horizon = best->time.land + safetySeconds;
                const double gap = remaining(horizon);
                const bool covered = std::any_of(incoming.begin(), incoming.end(), [&](const auto& c)
                {
                    return c.target == target.id && c.time.land <= horizon;
                });
                if (gap >= 0.0 && (!target.tpReady || covered)) break;
                assigned.insert(best->caster);
                requiredHp = std::max(requiredHp, target.biggest + target.damageRate * horizon);
                out.push_back({*best, target.hp < target.maximum, requiredHp});
                if (target.hp >= target.maximum) break;
                incoming.push_back(*best);
                // Another mage may cure simultaneously if this one still
                // leaves an emergency. Stop as soon as the estimate is safe.
                if (remaining(horizon) >= 0.0) break;
            }
        }
        return out;
    }
}
