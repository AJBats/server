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

// What a mob's area TP move does to a mage caught in it (RESEARCH §12.15,
// "How much a move matters"): the moves she ignores, the ones she avoids
// only when it is safe to, and -- every move not named here -- the ones she
// keeps out of. Read from every hostile area move's script on the era's
// data (2026-10-07; the user chose the borderline kinds): no HP damage, and
// nothing that stops a cast or a rest -- only HP loss ends a rest, any
// damage or a damage-over-time tick, never a debuff gaining. By the move's
// script name, so every copy of a move (its _dynamis and later ids) goes
// with it. A move added upstream is kept out of until it is named here.

#include "perimeter_math.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace cardian::perimeter
{
    // Harmless to a mage: stat, accuracy, evasion and defence downs,
    // Blindness and Flash (physical accuracy only), Amnesia (job abilities
    // and weapon skills, never spells), TP resets, moves that heal the
    // target; and the magic downs and a knockback alone (the user)
    inline constexpr std::array kIgnored = std::to_array<std::string_view>({
        "abrasive_tantara", // Amnesia
        "actinic_burst", // Flash
        "awful_eye", // STR Down (gaze)
        "boiling_point", // Magic Defense Down
        "bombilation", // TP reset
        "cackle", // Magic Attack/Accuracy/Defense Down
        "call_of_the_grave", // INT Down
        "carnal_nightmare", // TP reset
        "deadeye", // Defense Down, Magic Defense Down
        "debilitating_drone", // two random stat downs
        "demoralizing_roar", // Attack Down
        "dice_tp_loss", // TP reset
        "enervation", // Defense Down, Magic Defense Down
        "frightful_roar", // Defense Down
        "hi-freq_field", // Evasion Down
        "hypnotic_sway", // Amnesia (gaze)
        "infrasonics", // Evasion Down
        "ink_cloud", // Blindness
        "lunar_cry", // Accuracy Down, Evasion Down
        "nepenthean_hum", // Amnesia
        "noisome_powder", // Attack Down
        "nuclear_waste", // Elemental resistance down (-50)
        "obfuscate", // Flash
        "petal_pirouette", // TP reset
        "reaving_wind", // TP reset
        "reaving_wind_kb", // knockback only
        "regain_hp", // restores the target's HP and/or MP
        "regain_mp", // restores the target's HP and/or MP
        "rotten_stench", // Accuracy Down, Magic Accuracy Down
        "rumble", // Evasion Down
        "sand_blast", // Blindness
        "sandspray", // Blindness
        "sandstorm", // Blindness
        "scream", // MND Down
        "slipstream", // Accuracy Down
        "slipstream_dynamis", // Accuracy Down
        "smokebomb", // Blindness
        "sonic_boom", // Attack Down
        "sonic_boom_dynamis", // Attack Down
        "sonic_wave", // Defense Down
        "sonic_wave_dynamis", // Defense Down
        "sound_blast", // INT Down
        "stinking_gas", // VIT Down
        "subsonics", // Defense Down
        "subsonics_dynamis", // Defense Down
        "summer_breeze", // none on the target (mob erases its own debuffs or gains Regain)
        "target_analysis", // stat downs drained (STR..CHR)
        "tenebrous_mist", // TP reset
        "ultimate_terror", // stat downs drained (STR..CHR)
        "ultrasonics", // Evasion Down
        "ultrasonics_dynamis", // Evasion Down
        "viscid_emission", // Amnesia
        "whispers_of_ire_dynamis", // stat downs drained (STR..CHR)
        "wild_carrot", // restores the target's HP and/or MP
        "winds_of_oblivion", // Amnesia
        "wz_recover_all", // restores the target's HP and/or MP
    });

    // Avoided when it is safe to (the user): Slow, Weight, Bind, Max HP or
    // MP Down. She can still cast and rest in them
    inline constexpr std::array kSoft = std::to_array<std::string_view>({
        "axial_bloom", // Bind
        "binding_wave", // Bind
        "demonic_howl", // Slow
        "epoxy_spread", // Bind
        "filamented_hold", // Slow
        "filamented_hold_dynamis", // Slow
        "fuscous_ooze", // Weight, Encumbrance
        "gloeosuccus", // Slow
        "gravity_field", // Slow
        "great_bleat", // Max HP Down
        "intimidate", // Slow (gaze)
        "lead_breath", // Weight
        "lodesong", // Weight
        "mind_break", // Max MP Down (gaze)
        "mucus_spread", // Slow
        "murk", // Slow, Weight
        "riddle", // Max MP Down
        "sheep_bleat", // Slow
        "spider_web", // Slow
        "sticky_grenade", // Weight
        "sticky_thread", // Slow
        "sticky_thread_dynamis", // Slow
        "viscid_nectar", // Slow
        "viscid_secretion", // Slow, Weight
    });

    // How much a move matters to a mage, by its script name. Kept out of
    // (Hard) unless named above: every damaging move, Sleep, Silence, Stun,
    // Paralysis, Petrification, Charm, Doom, damage over time, MP drain,
    // and of the borderline kinds Curse and Bane, the dispels, the moves
    // that hand the mob's own ailments over (Wanion, Contamination) and
    // Rising Swell, which unequips her
    inline auto harmOf(const std::string_view name) -> Harm
    {
        if (std::find(kIgnored.begin(), kIgnored.end(), name) != kIgnored.end())
        {
            return Harm::None;
        }
        if (std::find(kSoft.begin(), kSoft.end(), name) != kSoft.end())
        {
            return Harm::Soft;
        }
        return Harm::Hard;
    }
} // namespace cardian::perimeter
