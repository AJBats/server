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

#include "common/cbasetypes.h"
#include "common/timer.h"

#include <array>
#include <chrono>
#include <functional>
#include <vector>

class CCharEntity;

// The server's yes-or-no questions to a player (the Link's OFFER and
// OFFER_ANSWER): something a feature holds back until he says which way it
// goes. The addon asks it on a screen of the Cardian menu. One stands for a
// player at a time. It ends one of four ways: his yes, his no, no answer
// within its patience (simulation time: a pause holds it), or his leaving the
// zone it was put in -- the last two count as his no. A player who signs out
// takes his question with him, and nothing comes of it; so does a question
// withdrawn for a newer one.
//
// A feature names its question by a kind (CL_OFFER_*) and says what follows an
// answer with a resolver for that kind. What the question is about rides with
// it as plain numbers, never a Lua reference: the resolver reads them back.
namespace pawn::offers
{
    struct Offer
    {
        uint32                  id     = 0;
        uint32                  player = 0; // charid
        uint8                   kind   = 0; // CL_OFFER_*
        uint16                  zone   = 0; // where it was put: leaving it lapses the question
        timer::time_point       deadline{};
        std::array<uint32, 4>   args{};  // the kind's own numbers: the purchase a warp's question holds
        std::vector<uint32>     members; // the characters it concerns: a warp's cardians
    };

    // What follows an answer to a question of this kind: yes, or no (his no,
    // or the question lapsing). PPlayer is in the world when it is called.
    using Resolver = std::function<void(CCharEntity* PPlayer, const Offer& offer, bool yes)>;
    void setResolver(uint8 kind, Resolver resolver);

    // Put the question to the player's addon. False when no addon is bound
    // to him, and nothing is recorded: the caller goes on as if he had never
    // been asked. A question he already had is withdrawn first, with nothing
    // coming of it.
    auto put(CCharEntity* PPlayer, Offer offer, std::chrono::seconds patience) -> bool;

    // His answer, by the question's id: CL_S_OK when it was his open question
    // (its resolver has run), CL_S_OFFER_GONE when it is not.
    auto answer(CCharEntity* PPlayer, uint32 id, bool yes) -> uint16;

    // Every second or so, from the zone tick: the questions that lapse, and
    // those whose player has gone
    void tick();
} // namespace pawn::offers
