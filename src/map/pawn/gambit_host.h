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
        // run as a cardian's, but for the behaviour rows, and his feet are
        // his client's: nothing walks him into range
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

        // Her tactician's Sneak Attack can go before her weapon skill now
        virtual auto SneakAttackNow(const CBattleEntity* PTarget) -> bool = 0;

        // The player's order from her command window waits as her next action
        // (a cardian's waiting out its own recast does not: her rows go on)
        virtual auto HasQueuedOrder() const -> bool = 0;
        virtual auto IsRetreating() const -> bool   = 0;
        // Her rest lets an action through (a cardian kneels by policy), and
        // as first aid weighs a kneeling caster: the seconds until she could
        // act, and what standing now costs her rest (rest_policy.h)
        virtual auto RestAllowsAction() const -> bool = 0;
        virtual auto RestReadyIn(double now) const -> double = 0;
        virtual auto RestInterruptionCost() const -> double = 0;
        // A cast could start from her body now, her rest aside (RestReadyIn
        // weighs that): a cardian stops to cast whenever she casts; a played
        // character once he has stood still and nothing else holds his rows
        // (an event, a mount, his logout, the quiet after a refusal)
        virtual auto StandsToCast() const -> bool = 0;
        // Mid-action: casting, readying, shooting, using an item
        virtual auto Acting() const -> bool = 0;
        // Not stunned, asleep or otherwise kept from acting
        virtual auto CanAct() -> bool = 0;
        // Her place among her party's cardians, which staggers their thinks
        virtual auto PartyPosition() const -> uint8 = 0;
        // One of the world's adventurers (pawn::world)
        virtual auto IsWorld() const -> bool = 0;
        // The live player her party has in her zone: `Ally: the player`
        virtual auto GetLivePlayer() const -> CCharEntity* = 0;
        // The one she is with, whose fights are the party's (the engage
        // door's leader): the player, a world camp's leader, or nobody; the
        // played character himself
        virtual auto Anchor() const -> CCharEntity* = 0;
        // A foe she holds off for now: her door passes it by
        virtual auto HoldingOff(const CBattleEntity* PFoe) const -> bool = 0;
        // The player sent her onto this mob himself: hers to fight, never to
        // attend
        virtual auto OrderedOnto(const CBattleEntity* PTarget) const -> bool = 0;
        // Whoever her fight's mob hates most
        virtual auto GetTopEnmity() const -> CBattleEntity* = 0;
        // Her rows' "mob" while she has no battle target: the fight she
        // attends or walks in on
        virtual auto PartyFightTarget() const -> CBattleEntity* = 0;
    };
} // namespace pawn
