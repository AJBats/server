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

#include "engage_math.h"
#include "gambit_ids.h"

#include "common/cbasetypes.h"
#include "data/enums/status_effect.h"
#include "entities/entity_id.h"

class CBattleEntity;
class CCharEntity;
class CSpell;
enum class SpellID : uint16;

namespace pawn
{
    // What the gambit engine (CGambits) asks of the character it runs for,
    // and how it acts through her. Two characters run gambits: a cardian,
    // whose controller moves and fights her (CPawnController), and a
    // character whose own client drives him (CLiveController), whose rows
    // use his hands and never his feet: the client owns where he stands.
    // The engine decides; the host answers and acts. Each action is true
    // when it started
    class GambitHost
    {
    public:
        virtual ~GambitHost() = default;

        // Whose the rows are: a character his own client drives. His rows
        // are orders alone -- no tactician line, no party role's rows, no
        // behaviour -- and his feet are his client's: nothing walks him
        // into range
        virtual auto OwnClient() const -> bool = 0;

        virtual auto Cast(EntityId target, SpellID spell) -> bool = 0;
        // A cast her tactician's conveyor assigned: the conveyor has already
        // weighed the party's other casts
        virtual auto CastAssigned(EntityId target, SpellID spell) -> bool = 0;
        // She could take this cast from the conveyor now: nothing keeps her
        // from acting and nothing of the player's waits ahead of it. A
        // cardian walks into range herself; a character his own client
        // drives must already stand where the cast would start
        virtual auto FreeToCast(CSpell* PSpell, CBattleEntity* PTarget) -> bool = 0;
        virtual auto Ability(EntityId target, uint16 ability) -> bool = 0;
        virtual auto WeaponSkill(EntityId target, uint16 skill) -> bool = 0;
        virtual auto RangedAttack(EntityId target) -> bool = 0;

        // The behaviour layer: what her behaviour rows assert this tick
        virtual void ClearGambitBehaviors()                         = 0;
        virtual void SetGambitBehavior(uint16 behavior, uint16 arg) = 0;

        // The pacer: the server would take a new action from her now
        virtual auto ReadyToAct() -> bool = 0;
        // Something on her refuses every job ability (Amnesia, Impairment)
        virtual auto AbilitiesShutOut() const -> bool = 0;
        // The stance buff on her now is the one the player's own order fired
        // (her tactician's stance never takes that one off), and the note of
        // one he just fired
        virtual auto PlayersBuff(uint16 ability, xi::StatusEffect effect) -> bool = 0;
        virtual void NoteOrderedStance(uint16 ability)                          = 0;
        // Her tactician's Sneak Attack can go before her weapon skill now
        virtual auto SneakAttackNow(const CBattleEntity* PTarget) -> bool = 0;

        // The player's order from her command window waits as her next action
        virtual auto HasQueuedOrder() const -> bool = 0;
        virtual auto IsRetreating() const -> bool   = 0;
        // Her rest lets an action through (a cardian kneels by policy)
        virtual auto RestAllowsAction() const -> bool = 0;
        // Mid-action: casting, readying, shooting, using an item
        virtual auto Acting() const -> bool = 0;
        // Not stunned, asleep or otherwise kept from acting
        virtual auto CanAct() -> bool = 0;
        // Her place among her party's cardians, which staggers their thinks
        virtual auto PartyPosition() const -> uint8 = 0;
        // Her tactician runs (tactician_line.h): her line row speaks
        virtual auto TacticianRuns() const -> bool = 0;
        // One of the world's adventurers (pawn::world)
        virtual auto IsWorld() const -> bool = 0;
        // The live player her party has in her zone: `Ally: the player`
        virtual auto GetLivePlayer() const -> CCharEntity* = 0;
        // Whoever her fight's mob hates most
        virtual auto GetTopEnmity() const -> CBattleEntity* = 0;
        // Whether a foe is of a finder's kind, as her engage door reads it
        virtual auto FoeOfKind(cardian::engage::Finder finder, CBattleEntity* PFoe) const -> bool = 0;
        // Her rows' "mob" while she has no battle target: the fight she
        // attends or walks in on
        virtual auto PartyFightTarget() const -> CBattleEntity* = 0;
    };
} // namespace pawn
