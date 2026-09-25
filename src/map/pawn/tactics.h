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

#include <optional>
#include <string>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CParty;
class CSpell;
enum class SpellID : uint16;

namespace pawn::tactics
{
    // The tactician (RESEARCH §12.4, §12.12): one per party, or per
    // alliance when there is one, found by the pointers the game already
    // holds. It owns the fight log; the MP bank, the conveyor and the rest
    // policy come in their slices. Pull-ticked: every cardian's controller
    // asks each tick, KO'd or not, and the first to ask advances the
    // party's picture.
    //
    // Slice 1 watches only. A scope with no real player in it (a world camp
    // of its own) is watched under pawn.TACTICS_WORLD alone.
    void tick(CCharEntity* PPawn, timer::time_point now);

    // The hitch (§12.12 item 10, the dispatcher shape): listeners on an
    // entity's AI handler that forward its events to the dispatcher, which
    // routes them by id to a tactician's fight log or drops them. A hitch
    // captures nothing, is idempotent (LSB replaces a listener with the
    // same identifier) and is never undone. A member is hitched when she
    // joins a scope, and again as she enters a zone (a relog or a re-stand
    // is a new entity under the same id); a mob when its record opens from
    // a member's event. Never call it from inside the entity's own trigger.
    void hitch(CBattleEntity* PEntity);

    // The module's OnCharZoneIn: a routed character's new body is hitched
    void zoneIn(CCharEntity* PChar);

    // The marked party-leave call (pawn::leftParty): out of her scope at
    // once, rather than after the grace a zoning member is given
    void memberLeft(const CBattleEntity* PMember, const CParty* PParty);

    // pawn.TACTICS_DEBUG, read once
    auto debug() -> bool;

    // A member or a mob by id, as her scope sees it: the alliance's
    // characters first, the zone-coded mob lookup for the rest
    auto entity(CCharEntity* PPawn, uint32 id) -> CBattleEntity*;

    // Her Role row says Support Mage (a real player never does)
    auto supportMage(CBattleEntity* PMember) -> bool;
    // She attends the fight on this mob from the perimeter rather than
    // fighting it: a Support Mage no Attack row of hers sends onto it
    // (CPawnController::AttendsFight; a real player never does)
    auto attendsFight(CBattleEntity* PMember, CBattleEntity* PMob) -> bool;

    // The conveyor's doors (RESEARCH §12.12 item 2; conveyor.h), for the
    // gambit engine. A scope no tactician watches has no conveyor, and its
    // rows cast as they always have
    auto has(const CCharEntity* PPawn) -> bool;

    // A spell row whose condition holds, fed to her scope's conveyor: a
    // null spell is "best cure", the tier the bank's. A debuff the bank
    // prices whose effect is on the target already, or nullified by what is
    // on it, is not fed -- the bank's own verdicts, so no row recasts what
    // is on. Whether the cast came straight back as hers, and what to cast
    struct Fed
    {
        bool        mine = false;
        SpellID     spell{};
        uint32      target = 0;
        std::string why;
    };
    auto feed(CCharEntity* PPawn, CSpell* PSpell, CBattleEntity* PTarget, uint32 row, const std::string& rowId) -> std::optional<Fed>;

    // Her standing assignment: the first need in her slot's order that is
    // hers; nothing offensive while she is not in the fight -- drawn, or
    // attending a mob that is engaged (RESEARCH §12.15)
    struct Assignment
    {
        SpellID     spell{};
        uint32      target = 0;
        std::string why;
        bool        approach = false;
        bool        emergency = false;
    };
    auto assignment(CCharEntity* PPawn, bool engaged) -> std::optional<Assignment>;

    // Emergency aid is measured/selected centrally in the party tick;
    // the Support Mage's ordinary needs are fed on her think.
    void roleThink(CCharEntity* PPawn, bool engaged);

    struct RestAdvice
    {
        bool wake = false;
        bool knownCost = false;
        double readyMp = 0.0;
        bool recover = false;
        bool criticalMp = false;
        double spentPerSecond = 0.0;
        double recoveredPerSecond = 0.0;
        double reserveMp = 0.0;
        double projectedMp = 0.0;
        double cheapestCureMp = 0.0;
        double fightSpentMp = 0.0;
        double recoveryTargetMp = 0.0;
        std::string why;
    };
    auto restAdvice(CCharEntity* PPawn) -> std::optional<RestAdvice>;
    void resetRestMemory(CCharEntity* PPawn);

    // The !tactics command: the caller's scope, its open and recent fights,
    // this zone's spot averages, the members' cure figures, this zone's
    // debuffs, and the plumbing's count
    auto lines(CCharEntity* PChar) -> std::vector<std::string>;
} // namespace pawn::tactics
