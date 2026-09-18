// Cardian: the party's choice of who gets up; the controller owns the body.
#pragma once

#include "conveyor.h"
#include "rest_math.h"
#include "tactics.h"
#include "cure_math.h"

#include <unordered_map>

namespace pawn::tactics
{
    class FightLog;

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
