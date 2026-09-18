// Cardian: shared resting lifecycle and recovery arithmetic (RESEARCH §12.8).
#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <vector>

namespace cardian::rest
{
    constexpr double kStandSeconds = 1.0;
    constexpr double kPaceSeconds = 60.0;

    // Callers supply attended fights, including zero spending. An absent
    // mage has no sample; she did not demonstrate a free fight.
    inline auto fightBudget(std::vector<double> costs) -> double
    {
        if (costs.empty()) return 0.0;
        const double mean = std::accumulate(costs.begin(), costs.end(), 0.0) / costs.size();
        std::sort(costs.begin(), costs.end(), std::greater<>());
        return mean + costs[costs.size() > 1 ? 1 : 0];
    }

    struct Rates
    {
        double spent = 0.0;     // MP/second, over the same wall-clock window
        double recovered = 0.0;
    };

    // Sample actual MP, including recovery during fights and time standing.
    // One bucket per second bounds memory even when many spells finish in a tick.
    // A full window denominator avoids inventing an enormous rate at startup.
    class Flow
    {
    public:
        void observe(const double now, const double mp, const double maximum)
        {
            if (!m_seen || now < m_at || now - m_at > kPaceSeconds || maximum != m_maximum)
            {
                m_samples.clear();
                m_seen = true;
                m_mp = mp;
                m_maximum = maximum;
            }
            const double delta = mp - m_mp;
            const double bucket = std::floor(now);
            if (delta != 0.0)
            {
                if (m_samples.empty() || m_samples.back().at != bucket)
                {
                    m_samples.push_back({bucket, {}});
                }
                auto& rates = m_samples.back().rates;
                rates.spent += std::max(0.0, -delta);
                rates.recovered += std::max(0.0, delta);
            }
            m_mp = mp;
            m_at = now;
            expire(now);
        }

        auto rates(const double now) -> Rates
        {
            expire(now);
            Rates out;
            for (const auto& sample : m_samples)
            {
                out.spent += sample.rates.spent / kPaceSeconds;
                out.recovered += sample.rates.recovered / kPaceSeconds;
            }
            return out;
        }

    private:
        void expire(const double now)
        {
            while (!m_samples.empty() && m_samples.front().at <= now - kPaceSeconds)
            {
                m_samples.pop_front();
            }
        }
        struct Sample { double at; Rates rates; };
        std::deque<Sample> m_samples;
        bool m_seen = false;
        double m_at = 0.0;
        double m_mp = 0.0;
        double m_maximum = 0.0;
    };

    struct Pace
    {
        bool recover = false;
        bool critical = false;
        double reserve = 0.0;
        double projected = 0.0;
        double target = 0.0;
    };

    inline auto pacing(const double mp, const double maximum, const Rates rates,
                       const double fightBudget, const bool knownBudget, const bool resting,
                       const double firstRecovery, const double firstTickMp, const double cheapestCure,
                       const double spentInFight = 0.0) -> Pace
    {
        Pace out;
        if (maximum <= 0.0)
        {
            return out;
        }
        // Fight cost survives long downtime when recent rates have faded.
        // Cap an impossible budget to capacity BEFORE subtracting what this
        // fight has already spent. A spell does not start a whole new fight.
        const double remainingBudget = knownBudget ? std::max(0.0, std::min(fightBudget, maximum) - spentInFight) : 0.0;
        out.reserve = std::clamp(knownBudget ? remainingBudget : 0.25 * maximum, 0.25 * maximum, 0.75 * maximum);
        // 2026-09-18: removed an extra 20-second work horizon. Project only
        // through the real first recovery delay; revisit if playtests show
        // MP running out before that tick, using the recorded rates/budget.
        out.projected = mp - std::max(0.0, rates.spent - rates.recovered) * firstRecovery;
        out.critical = mp <= 0.10 * maximum || mp < cheapestCure;
        out.target = std::min(maximum, std::max(remainingBudget, out.reserve + std::max(0.10 * maximum, firstTickMp)));
        if (resting)
        {
            // Leave on a real tick once both pace and reserve recover. At
            // full MP, capped recovery cannot build any more rate credit.
            out.recover = mp < maximum && (out.critical || mp < out.target || rates.recovered < rates.spent);
        }
        else
        {
            out.recover = mp < maximum && (out.critical || out.projected <= out.reserve);
        }
        return out;
    }

    // Emergency healer selection still sees actual recasts. This compares
    // against recovery timing, not the removed fixed 20-second ordinary-work
    // forecast, and never vetoes rest for an ordinary upcoming spell.
    inline auto cureBeforeRecovery(const double recast, const double cast, const double firstRecovery) -> bool
    {
        return recast + cast <= firstRecovery;
    }

    // Healing's real tick count: tick one is empty, tick two recovers 12 MP.
    inline auto mpAtTick(const int tick, const int clearMind, const int mpHeal) -> double
    {
        return tick < 2 ? 0.0 : std::max(0, 12 + (tick - 2) * (1 + clearMind) + mpHeal);
    }

    inline auto timeToReady(const double missing, const int ticks, const double nextTick,
                            const double interval, const int clearMind, const int mpHeal) -> double
    {
        if (missing <= 0.0)
        {
            return 0.0;
        }
        if (interval <= 0.0)
        {
            return std::numeric_limits<double>::infinity();
        }
        double gained = 0.0;
        for (int i = 1; i <= 10000; ++i)
        {
            gained += mpAtTick(ticks + i, clearMind, mpHeal);
            if (gained >= missing)
            {
                return std::max(0.0, nextTick) + (i - 1) * interval;
            }
        }
        return std::numeric_limits<double>::infinity();
    }

    // Recovery forfeited over two minutes by standing, casting, then re-kneeling.
    inline auto interruptionCost(const int ticks, const double nextTick, const double interval,
                                 const int clearMind, const int mpHeal) -> double
    {
        if (interval <= 0.0)
        {
            return 0.0;
        }
        double keep = 0.0;
        double restart = 0.0;
        int tick = ticks;
        for (double at = std::max(0.0, nextTick); at <= 120.0; at += interval)
        {
            keep += mpAtTick(++tick, clearMind, mpHeal);
        }
        tick = 0;
        for (double at = kStandSeconds + 3.0 + interval; at <= 120.0; at += interval)
        {
            restart += mpAtTick(++tick, clearMind, mpHeal);
        }
        return std::max(0.0, keep - restart);
    }

    // Rest-with-leader is a request source, not another writer of the kneel.
    struct Follow
    {
        bool observed = false;
        bool held = false;
        double due = 0.0;

        auto request(const bool enabled, const bool leaderDown, const double now, const double beat) -> bool
        {
            if (!enabled)
            {
                *this = {};
                return false;
            }
            if (now >= due)
            {
                held = observed;
            }
            if (leaderDown != observed)
            {
                observed = leaderDown;
                due = now + beat;
            }
            if (now >= due)
            {
                held = observed;
            }
            return held;
        }
    };

    enum class Decision { Stand, StayUp, Kneel, StayDown };

    struct Facts
    {
        double now = 0.0;
        bool resting = false;
        bool want = false;
        bool withPlayer = false; // explicit Rest With Player request, independent of MP pacing
        bool campClear = false; // Support Mage: no party enemy within the camp's engagement boundary
        bool mpMissing = false;
        bool urgent = false;
        bool blocked = false; // unsafe, acting, ordered away, unable to recover
        bool moving = false;
        bool routinePosition = false; // an ongoing camp rest may defer this move
        bool recovered = false;
        bool tickLanded = false;
    };

    struct State
    {
        // 2026-09-18: removed the 20-second standing/work window and 2-second
        // post-action quiet period. MP pacing owns ordinary rest entry once
        // the body is free. If bobbing persists, inspect pacing before adding
        // a second policy timer (RESEARCH §12.8 / ROADMAP slice 5).
        // Keep the physical rise gate; its duration still needs live validation.
        double actAfter = 0.0;
        bool observedDown = false;
        bool wantsDown = false;

        void stood(const double now)
        {
            observedDown = false;
            wantsDown = false;
            actAfter = now + kStandSeconds;
        }

        void observe(const bool down, const double now)
        {
            if (observedDown && !down)
            {
                stood(now); // damage or another engine effect interrupted this rest
            }
            observedDown = down;
        }

        auto canAct(const double now, const bool down) const -> bool
        {
            return !wantsDown && !down && now >= actAfter;
        }

        auto decide(const Facts& f) -> Decision
        {
            observe(f.resting, f.now);
            const bool movementRequiresStand = f.moving && !(f.resting && f.routinePosition);
            // A player's rest is another request to this lifecycle, including
            // for Support Mage. Reaching her MP target cannot cancel it.
            // While camp has no enemy to act on, preserve an existing rest's
            // recovery ramp until MP is full. This never starts a new rest or
            // forces a wake when an enemy arrives; ordinary pacing resumes.
            const bool preserveRecovery = f.resting && f.campClear && f.mpMissing;
            const bool up = f.urgent || f.blocked || movementRequiresStand || (!f.want && !f.withPlayer) ||
                            (f.resting && f.recovered && f.tickLanded && !f.withPlayer && !preserveRecovery);
            wantsDown = !up;
            if (f.resting)
            {
                if (up)
                {
                    stood(f.now);
                    return Decision::Stand;
                }
                return Decision::StayDown;
            }
            if (up || f.now < actAfter)
            {
                wantsDown = false;
                return Decision::StayUp;
            }
            observedDown = true;
            return Decision::Kneel;
        }
    };
}
