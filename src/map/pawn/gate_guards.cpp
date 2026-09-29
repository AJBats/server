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

#include "gate_guards.h"

#include "common/utils.h"
#include "entities/char_entity.h"
#include "zone.h"

#include <array>
#include <string>

namespace pawn::guards
{
    namespace
    {
        constexpr float kGuardReach = 8.0f; // yalms, the player to a guard

        constexpr uint8 kSandoria = 0; // xi.nation
        constexpr uint8 kBastok   = 1;
        constexpr uint8 kWindurst = 2;
        constexpr uint8 kOther    = 4;
        constexpr uint8 kCity     = 1; // xi.conquest.guard
        constexpr uint8 kForeign  = 2;

        constexpr std::array kGuards{
            Guard{ "Achantere_TK", kSandoria, kCity, "Northern_San_dOria" },
            Guard{ "Alrauverat", kOther, kCity, "Lower_Jeuno" },
            Guard{ "Aravoge_TK", kSandoria, kCity, "Southern_San_dOria" },
            Guard{ "Arpevion_TK", kSandoria, kCity, "Southern_San_dOria" },
            Guard{ "Chapal-Afal_WW", kWindurst, kForeign, "Northern_San_dOria" },
            Guard{ "Crying_Wind_IM", kBastok, kCity, "Bastok_Mines" },
            Guard{ "Emitt", kOther, kCity, "Upper_Jeuno" },
            Guard{ "Flying_Axe_IM", kBastok, kCity, "Port_Bastok" },
            Guard{ "Glarociquet_TK", kSandoria, kForeign, "Metalworks" },
            Guard{ "Harara_WW", kWindurst, kCity, "Windurst_Woods" },
            Guard{ "Kochahy-Muwachahy", kOther, kCity, "Port_Jeuno" },
            Guard{ "Lexun-Marixun_WW", kWindurst, kForeign, "Metalworks" },
            Guard{ "Milma-Hapilma_WW", kWindurst, kCity, "Port_Windurst" },
            Guard{ "Morlepiche", kOther, kCity, "RuLude_Gardens" },
            Guard{ "Panoquieur_TK", kSandoria, kForeign, "Windurst_Woods" },
            Guard{ "Puroiko-Maiko_WW", kWindurst, kCity, "Windurst_Waters" },
            Guard{ "Rabid_Wolf_IM", kBastok, kCity, "Bastok_Markets" },
            Guard{ "Sachetan_IM", kBastok, kForeign, "Port_Windurst" },
            Guard{ "Yevgeny_IM", kBastok, kForeign, "Northern_San_dOria" },
        };
    } // namespace

    auto guardNear(const CCharEntity* PPlayer) -> const Guard*
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return nullptr;
        }
        // Only the names that stand in his zone are asked for: the zone keeps
        // the answer to every name it is asked, misses included
        const std::string zoneName = PPlayer->loc.zone->getName();
        const Guard*      PNearest = nullptr;
        float             nearest  = kGuardReach;
        for (const auto& guard : kGuards)
        {
            if (guard.zone != zoneName)
            {
                continue;
            }
            for (const auto* PNpc : PPlayer->loc.zone->queryEntitiesByName(std::string(guard.name)))
            {
                if (PNpc == nullptr)
                {
                    continue;
                }
                if (const float away = distance(PPlayer->loc.p, PNpc->loc.p); away <= nearest)
                {
                    PNearest = &guard;
                    nearest  = away;
                }
            }
        }
        return PNearest;
    }
} // namespace pawn::guards
