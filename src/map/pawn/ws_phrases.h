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

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The weapon skills of the game's auto-translate dictionary, so a cardian's
// party chat names one as a player's macro does, in the game's braces and
// each reader's language: group 33, entry 1 onward, in the client's order.
// A copy of src/map/autotranslate.cpp's group 33 (its keys of type 2,
// language 2), compared at every merge from base as the other copies are
namespace cardian::chat
{
    inline constexpr uint8_t kWeaponSkillGroup = 33;

    inline constexpr std::array<std::string_view, 217> kWeaponSkillPhrases{
        "Slug Shot",
        "Shoulder Tackle",
        "Red Lotus Blade",
        "Spirits Within",
        "Backhand Blow",
        "Power Slash",
        "Fast Blade",
        "Flat Blade",
        "Freezebite",
        "Brainshaker",
        "Frostbite",
        "Heavy Swing",
        "Heavy Swing",
        "Raiden Thrust",
        "Burning Blade",
        "Raging Axe",
        "Hard Slash",
        "Rock Crusher",
        "Wasp Sting",
        "Raging Fists",
        "Spinning Attack",
        "One Inch Punch",
        "Cyclone",
        "Sniper Shot",
        "Energy Drain",
        "Dancing Edge",
        "Seraph Blade",
        "Circle Blade",
        "Gale Axe",
        "Shining Blade",
        "Earth Crusher",
        "Armor Break",
        "Iron Tempest",
        "Leg Sweep",
        "Avalanche Axe",
        "Gust Slash",
        "Combo",
        "Thunder Thrust",
        "Viper Bite",
        "Shining Strike",
        "Energy Steal",
        "Shadow of Death",
        "Shadowstitch",
        "Sturmwind",
        "Sturmwind",
        "Starburst",
        "Starlight",
        "Smash Axe",
        "Slice",
        "Seraph Strike",
        "Dark Harvest",
        "Double Thrust",
        "Nightmare Scythe",
        "Shield Break",
        "Judgment",
        "Blade: Jin",
        "Vorpal Blade",
        "Tachi: Hobaku",
        "Howling Fist",
        "Tachi: Kagero",
        "Tachi: Jinpu",
        "Tachi: Koki",
        "Tachi: Yukikaze",
        "Moonlight",
        "Blade: Ei",
        "True Strike",
        "Tachi: Enpi",
        "Sunburst",
        "Shell Crusher",
        "Full Swing",
        "Flaming Arrow",
        "Piercing Arrow",
        "Dulling Arrow",
        "Sidewinder",
        "Hot Shot",
        "Split Shot",
        "Skullbreaker",
        "Weapon Break",
        "Shockwave",
        "Crescent Moon",
        "Sickle Moon",
        "Spinning Axe",
        "Rampage",
        "Tachi: Goten",
        "Keen Edge",
        "Raging Rush",
        "Spinning Scythe",
        "Vorpal Scythe",
        "Guillotine",
        "Penta Thrust",
        "Vorpal Thrust",
        "Blade: Rin",
        "Blade: Retsu",
        "Blade: Teki",
        "Blade: To",
        "Blade: Chi",
        "Calamity",
        "Skewer",
        "Arching Arrow",
        "Cross Reaper",
        "Blade: Ten",
        "Blast Arrow",
        "Blast Shot",
        "Dragon Kick",
        "Full Break",
        "Heavy Shot",
        "Heavy Shot",
        "Hexa Strike",
        "Spirit Taker",
        "Swift Blade",
        "Tachi: Gekko",
        "Wheeling Thrust",
        "Shark Bite",
        "Spinning Slash",
        "Mistral Axe",
        "Savage Blade",
        "Asuran Fists",
        "Evisceration",
        "Evisceration",
        "Decimation",
        "Spiral Hell",
        "Black Halo",
        "Steel Cyclone",
        "Ground Strike",
        "Impulse Drive",
        "Impulse Drive",
        "Retribution",
        "Blade: Ku",
        "Tachi: Kasha",
        "Empyreal Arrow",
        "King's Justice",
        "Ascetic's Fury",
        "Mystic Boon",
        "Vidohunir",
        "Vidohunir",
        "Death Blossom",
        "Mandalic Stab",
        "Atonement",
        "Insurgency",
        "Primal Rend",
        "Mordant Rime",
        "Trueflight",
        "Tachi: Rana",
        "Blade: Kamu",
        "Drakesbane",
        "Garland of Bliss",
        "Expiacion",
        "Leaden Salute",
        "Stringing Pummel",
        "Pyrrhic Kleos",
        "Omniscience",
        "Detonator",
        "Shijin Spiral",
        "Exenterator",
        "Requiescat",
        "Resolution",
        "Ruinator",
        "Upheaval",
        "Entropy",
        "Stardiver",
        "Blade: Shun",
        "Tachi: Shoha",
        "Realmrazer",
        "Shattersoul",
        "Apex Arrow",
        "Last Stand",
        "Tornado Kick",
        "Aeolian Edge",
        "Sanguine Blade",
        "Herculean Slash",
        "Bora Axe",
        "Fell Cleave",
        "Fell Cleave",
        "Infernal Scythe",
        "Sonic Thrust",
        "Flash Nova",
        "Flash Nova",
        "Cataclysm",
        "Blade: Yu",
        "Tachi: Ageha",
        "Refulgent Arrow",
        "Numbing Shot",
        "Victory Smite",
        "Rudra's Storm",
        "Chant du Cygne",
        "Torcleaver",
        "Cloudsplitter",
        "Ukko's Fury",
        "Quietus",
        "Camlann's Torment",
        "Blade: Hi",
        "Tachi: Fudo",
        "Dagan",
        "Myrkr",
        "Jishnu's Radiance",
        "Wildfire",
        "Dimidiation",
        "Exudation",
        "Uriel Blade",
        "Glory Slash",
        "Tartarus Torpor",
        "Knights of Rotund",
        "Final Heaven",
        "Final Heaven",
        "Mercy Stroke",
        "Knights of Round",
        "Scourge",
        "Onslaught",
        "Metatron Torment",
        "Catastrophe",
        "Geirskogul",
        "Blade: Metsu",
        "Tachi: Kaiten",
        "Randgrith",
        "Gate of Tartarus",
        "Namas Arrow",
        "Coronach",
    };

    // A name as both spellings compare: the server's "blade_chi" and the
    // dictionary's "Blade: Chi" alike, its letters and digits in lower case
    inline auto phraseKey(const std::string_view name) -> std::string
    {
        std::string out;
        for (const char c : name)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0)
            {
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
        }
        return out;
    }

    // A weapon skill's entry in group 33, by its name; 0 for none there
    inline auto weaponSkillEntry(const std::string_view name) -> uint8_t
    {
        const auto key = phraseKey(name);
        for (std::size_t i = 0; i < kWeaponSkillPhrases.size(); ++i)
        {
            if (phraseKey(kWeaponSkillPhrases[i]) == key)
            {
                return static_cast<uint8_t>(i + 1);
            }
        }
        return 0;
    }
} // namespace cardian::chat
