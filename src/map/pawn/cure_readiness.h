// Cardian: read-only Cure measurements; policy and casting live elsewhere.
#pragma once

#include "conveyor.h"
#include "cure_math.h"

namespace pawn::tactics
{
    auto measureCures(const Conveyor::Scope& scope, double now) -> std::vector<cardian::cure::Option>;
}
