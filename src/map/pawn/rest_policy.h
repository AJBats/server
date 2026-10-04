// Cardian: the party's choice of who gets up; the controller owns the body.
#pragma once

#include "conveyor.h"
#include "rest_math.h"
#include "tactics.h"
#include "cure_math.h"
#include "common/timer.h"

#include <string_view>
#include <unordered_map>

class CBattleEntity;
class CStatusEffect;

namespace pawn::tactics
{
    class FightLog;

    // A character's kneel as the rest lifecycle reads it (rest_math.h
    // State), the same body rules for a cardian and a played character:
    // the rest clock's seconds; Healing's ticks so far and the seconds to
    // the next and between them (zero while standing); and what standing
    // now costs (the tick about to land, lost)
    auto restSeconds(timer::time_point time) -> double;
    struct KneelClock
    {
        bool   down     = false;
        int    ticks    = 0;
        double next     = 0.0;
        double interval = 0.0;
    };
    auto kneelClock(const CStatusEffect* healing, double now) -> KneelClock;
    auto restInterruptionCost(CBattleEntity* PBody) -> double;
    // Her kneel's say over an action and a rise, on her rest lifecycle: an
    // action may go, the seconds until she could act, and standing her up
    // now -- false while she is not down, or must finish kneeling first
    auto kneelAllowsAction(const cardian::rest::State& state, const CBattleEntity* PBody) -> bool;
    auto kneelReadyIn(const cardian::rest::State& state, const CBattleEntity* PBody, double now) -> double;
    auto standFromKneel(cardian::rest::State& state, CBattleEntity* PBody, std::string_view why) -> bool;

    class RestPlanner
    {
    public:
        void tick(FightLog& log, const Conveyor::Scope& scope, double now, std::span<const cardian::cure::Choice> emergency);
        void reset(uint32 closedCount);
        void observe(CBattleEntity* member, double now);
        auto advice(uint32 id) const -> std::optional<RestAdvice>;
        auto lines(const Conveyor::Scope& scope) const -> std::vector<std::string>;

    private:
        uint32 m_since = 0;
        std::unordered_map<uint32, RestAdvice> m_advice;
        struct Recovery
        {
            cardian::rest::Flow flow;
            const CBattleEntity* body = nullptr; // identity only; never dereferenced
            uint16 zone = 0;
            double logAt = 0.0;
            bool recovering = false;
        };
        std::unordered_map<uint32, Recovery> m_recovery;
    };
}
