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

#pragma once

// A cardian's order as the pawn code keeps it (pawn_controller.h, her queued
// order) and as the Cardian Link carries it. The pawn code names an order by a
// key: the gambit catalogue's kind:mode:id, item:<id>, attack, disengage, move,
// movewait, rest:<percent> or rescue. The Link carries typed fields (cl_action); these
// two cross between them at the Link's edge, so no key text crosses the wire.

#include "cardian_link_messages.h"

#include <cstdio>
#include <string>

namespace pawn
{
    // The fields for a key; CL_AK_NONE for a key that names no order
    inline auto actionOfKey(const std::string& key) -> cl_action
    {
        cl_action action{};
        if (key == "attack")
        {
            action.kind = CL_AK_ATTACK;
        }
        else if (key == "disengage")
        {
            action.kind = CL_AK_DISENGAGE;
        }
        else if (key == "move")
        {
            action.kind = CL_AK_MOVE;
        }
        else if (key == "movewait")
        {
            action.kind = CL_AK_MOVE_WAIT;
        }
        else if (key == "rescue")
        {
            action.kind = CL_AK_RESCUE;
        }
        else if (unsigned id = 0; std::sscanf(key.c_str(), "item:%u", &id) == 1 && id <= UINT16_MAX)
        {
            action.kind = CL_AK_ITEM;
            action.id   = static_cast<uint16_t>(id);
        }
        else if (unsigned percent = 0; std::sscanf(key.c_str(), "rest:%u", &percent) == 1 && percent >= 1 && percent <= 100)
        {
            action.kind = CL_AK_REST;
            action.id   = static_cast<uint16_t>(percent);
        }
        else if (unsigned kind = 0, mode = 0, id = 0; std::sscanf(key.c_str(), "%u:%u:%u", &kind, &mode, &id) == 3 &&
                                                      kind >= CL_AK_RANGED && kind <= CL_AK_WEAPONSKILL && mode <= UINT8_MAX && id <= UINT16_MAX)
        {
            action.kind = static_cast<uint8_t>(kind);
            action.mode = static_cast<uint8_t>(mode);
            action.id   = static_cast<uint16_t>(id);
        }
        return action;
    }

    // The key for the fields; "" for fields that name no cardian's order (none,
    // an unknown kind, a player's own command, a rescue: his Rescue comes by its
    // own message)
    inline auto keyOfAction(const cl_action& action) -> std::string
    {
        switch (action.kind)
        {
            case CL_AK_RANGED:
            case CL_AK_MAGIC:
            case CL_AK_ABILITY:
            case CL_AK_WEAPONSKILL:
                return std::to_string(action.kind) + ":" + std::to_string(action.mode) + ":" + std::to_string(action.id);
            case CL_AK_ITEM:
                return "item:" + std::to_string(action.id);
            case CL_AK_ATTACK:
                return "attack";
            case CL_AK_DISENGAGE:
                return "disengage";
            case CL_AK_MOVE:
                return "move";
            case CL_AK_MOVE_WAIT:
                return "movewait";
            case CL_AK_REST:
                return action.id >= 1 && action.id <= 100 ? "rest:" + std::to_string(action.id) : std::string();
            default:
                return {};
        }
    }
} // namespace pawn
