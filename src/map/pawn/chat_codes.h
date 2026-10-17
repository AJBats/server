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

#include <cstdint>
#include <string>

// What a cardian's chat line carries besides plain words, written as the
// game client writes it, so the client that reads the line renders it as a
// player's: both encodings were measured from a real client's party chat
// (2026-10-16), and the party chat path (role_support.h sayParty, the same
// IPC message a player's own party chat rides) hands the bytes on untouched.
namespace cardian::chat
{
    // The game's call sounds, <call0> to <call20>, by their own numbers: the
    // ones the Settings calls offer. kNoCall is none (the Link's
    // CL_CALL_NONE), apart from Call 0
    constexpr uint8_t kCallCount = 21;
    constexpr uint8_t kNoCall    = 0xFF;

    // A call sound inside a chat line. The client sends one as the plain
    // text "<callN>" -- the player's "<call11>" reached the server as the
    // bytes 3C 63 61 6C 6C 31 31 3E -- and the client that receives the line
    // turns the text into the sound. kNoCall, or any number the game has no
    // call for, is no call: nothing
    inline auto callBytes(const uint8_t call) -> std::string
    {
        if (call >= kCallCount)
        {
            return {};
        }
        return "<call" + std::to_string(call) + ">";
    }

    // A call kept as given when the game has it, else none
    inline auto keptCall(const uint8_t call) -> uint8_t
    {
        return call < kCallCount ? call : kNoCall;
    }

    // An auto-translate phrase inside a chat line: six raw bytes, FD 02 02,
    // the phrase's group and entry, FD (the player's "Incredibly tough"
    // reached the server as FD 02 02 04 03 FD). The receiving client shows
    // the phrase in its own language, in the game's green and red braces.
    // LSB keeps the whole dictionary in autotranslate.cpp (its key: type |
    // lang << 8 | group << 16 | entry << 24)
    inline auto autoTranslate(const uint8_t group, const uint8_t entry) -> std::string
    {
        return std::string{ static_cast<char>(0xFD), static_cast<char>(0x02), static_cast<char>(0x02), static_cast<char>(group), static_cast<char>(entry),
                            static_cast<char>(0xFD) };
    }
} // namespace cardian::chat
