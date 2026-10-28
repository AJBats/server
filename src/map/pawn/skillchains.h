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

#include "skillchain_math.h"

#include "common/cbasetypes.h"

#include <optional>
#include <string>
#include <vector>

class CBattleEntity;
class CCharEntity;

// The party's skillchains and the bursts on them (ROADMAP, Road to subjob
// item 6, slice 3h; the rules are skillchain_math.h): the player's plan, the
// options his Shortcuts menu lists, each member's place in the plan, what is
// on a mob, the plan's TP window and its worth on a mob, his Skillchain now,
// what the party's cardians say of it, and the Black Mage's telepathy --
// when the chain lands. The game's own rules are asked of the server
// (battleutils, and its skillchain damage through tactics_bank.lua), never
// copied.
namespace pawn::chains
{
    // The server's skillchain rules as the pure rules take them:
    // FormSkillchain on an opening's properties and a closing weapon skill's,
    // GetSkillchainTier, and GetSkillchainMagicElement as a set of element
    // bits (skillchain_math.h elementBit)
    auto forms(const cardian::chains::Properties& opening, const cardian::chains::Properties& closing) -> uint8;
    auto tierOf(uint8 chain) -> uint8;
    auto elementsOf(uint8 chain) -> uint16;

    // A character's weapon skills she can use now -- learned, with the
    // weapon in her hand -- with their properties, as the server has them
    auto skillsOf(CCharEntity* PChar) -> std::vector<cardian::chains::Skill>;

    // What is on a mob: an opening or a chain (the Skillchain effect), how
    // old and how long it has left; none when nothing is
    auto resonanceOn(CBattleEntity* PMob) -> std::optional<cardian::chains::Resonance>;

    // The player's plan (the Shortcuts menu's pick): the skillchain his
    // party's tacticians' weapon skills make. One per player, in memory for
    // his session, as his strategy is (skillchain_math.h PlanBook)
    using Plan = cardian::chains::Plan;
    auto planOf(uint32 ownerCharID) -> std::optional<Plan>;

    // The skillchains his party can make now (skillchain_math.h options),
    // with the names the menu shows. The members: the player, and each
    // cardian of his party in his zone whose tactician's Weapon skill
    // (best) runs (CGambits::OffersWeaponSkill) -- a plain weapon skill row
    // would not follow a plan; another real player of his party as he is
    struct Offered
    {
        cardian::chains::Option option;
        std::string             opener;
        std::string             closer;
        std::string             burster;
    };
    auto optionsFor(CCharEntity* POwner) -> std::vector<Offered>;

    // His pick: those two members' weapon skills as his plan, or none with
    // opener 0. CL_S_OK; CL_S_NOT_IN_PARTY for a member not among those the
    // options read; CL_S_NO_SKILLCHAIN for weapon skills that are not hers
    // now or make no chain
    auto choose(CCharEntity* POwner, uint32 opener, uint16 openerSkill, uint32 closer, uint16 closerSkill) -> uint16;

    // His Skillchain now (the Shortcuts menu, the user, 2026-10-16): on the
    // mob being fought -- his own, else the one his plan's opener or closer
    // fights -- the plan's members open and close as soon as they can,
    // whatever its worth says, for that fight. CL_S_OK and the mob;
    // CL_S_NO_PLAN with no plan; CL_S_NO_FIGHT with no mob being fought
    struct Forced
    {
        uint16         status = 0;
        CBattleEntity* PMob   = nullptr;
    };
    auto skillchainNow(CCharEntity* POwner) -> Forced;

    // A member out of a party (pawn::leftParty, called for a member who
    // leaves, is kicked, is released or sent away, or whose party breaks up;
    // never for one who zones or is knocked out): a plan naming her ends,
    // and so does a player's own as he leaves
    void memberLeft(const CBattleEntity* PMember);
    // A mob dead (the fight log's death event): his Skillchain now on it
    // ends with that fight, and the save line's once a fight with it
    void mobDied(uint32 mobId);
    // A player signing out: his plan ends with his session, and all the rest
    // kept for him -- his Skillchain now, his cardians' ask (pawn::signOutClub)
    void signedOut(uint32 playerCharID);

    // A member's place in the plan of the party she fights for, on her mob:
    // her seat and her weapon skill in it, the chain, and her partner --
    // whether he fights the same mob and follows the plan (a cardian's
    // tactician's Weapon skill (best) runs, her gambits on; the plan's
    // weapon skill his now), and with the TP for his weapon skill. Every
    // member, in it or not, knows what the plan's opening looks like and
    // whether its closer plays his part on her mob, and the plan on her mob
    // as the tactician reads it: saved for the next fight (its worth), his
    // Skillchain now, and when it can start; and a member of it, whether a
    // weapon skill of hers alone now is spare (skillchain_math.h spareWs)
    struct Part
    {
        bool                        planned       = false; // her player has a plan
        cardian::chains::Seat       seat          = cardian::chains::Seat::None;
        uint16                      skill         = 0;
        uint8                       chain         = 0;
        bool                        partnerFights = false;
        bool                        partnerReady  = false;
        std::string                 partner;
        cardian::chains::Properties planOpening{};
        bool                        planCloserFights = false;
        std::string                 planCloser;
        bool                        saving     = false;
        bool                        forced     = false;
        bool                        planComing = false;
        std::optional<double>       startIn;
        std::optional<double>       spareIn;
    };
    auto partOf(CCharEntity* PMember, CBattleEntity* PMob) -> Part;

    // A member's hold for the plan (skillchain_math.h kPlanHoldSafety):
    // noted as her weapon skill holds for it or stops, and how long it has
    // lasted -- kept here so the others' readings of her (othersClose, the
    // forecast) ask the same rule her own weapon skill does
    void noteHold(const CBattleEntity* PMember, bool holding);
    auto heldForPlan(const CBattleEntity* PMember) -> double;
    // A member's weapon skill gone out on a mob: a spare one counts against
    // kSparesPerHold, and playing her part in the plan (its opening, its own
    // close) starts the count again, as does another mob
    void noteSpent(const CBattleEntity* PMember, const CBattleEntity* PMob, bool spare, bool plays);

    // Telepathy: the chain open on a mob, or expected on it, as the Black
    // Mage's nukes read it -- a plan's only while both its members play
    // their part on the mob, not on a mob it does not fit (its worth; his
    // Skillchain now aside), and not once both have had the TP for longer
    // than the opening takes with none come. `open`: a burst window open now
    // and the seconds it has left; `in`: a chain expected in this many
    // seconds, from the plan's TP window; `count`: the chain's links, which
    // the burst's damage grows with; `how`: how it was read, for the map
    // log. Every member asked about has her TP sampled
    struct Forecast
    {
        uint8                 chain    = 0;
        uint16                elements = 0;
        uint8                 count    = 1;
        std::optional<double> open;
        std::optional<double> in;
        std::string           how;
    };
    auto forecast(CCharEntity* PNuker, CBattleEntity* PMob) -> std::optional<Forecast>;

    // Somebody else will use what is on a mob, so a member who cannot close
    // it keeps her weapon skill off it (skillchain_math.h wsCall): another
    // member fighting it, with the TP, has a weapon skill that chains with
    // it; or, for a chain, a member of the party at the fight on it can
    // burst its elements, with the MP for one
    auto othersClose(CCharEntity* PSelf, CBattleEntity* PMob, const cardian::chains::Resonance& r) -> bool;
    auto burstComing(CCharEntity* PSelf, CBattleEntity* PMob, const cardian::chains::Resonance& r) -> bool;

    // A member's TP, sampled for her rate (skillchain_math.h TpClock); every
    // gambit engine's tick samples its own, and the forecast those it asks about
    void noteTp(CBattleEntity* PMember);

    // A member's weapon skill landed on a mob for this much (the fight log,
    // as it lands), or missed (0, its use with no damage): what the chain's
    // worth prices her weapon skill at
    void wsLanded(const CBattleEntity* PMember, uint16 skill, int32 damage);
    // A weapon skill of hers the game reports as dealing nothing: a miss, a 0
    // in its price -- unless the same weapon skill of hers landed damage this
    // moment, a skillchain's close, which the game reports as nothing once
    // the chain's own damage lands
    void wsMissed(const CBattleEntity* PMember, uint16 skill);
    // What a member's weapon skill deals this mob, for comparing hers: the
    // server's own weapon skill at her TP (tactics_bank.lua wsExpected),
    // else her landings of it, else nothing. Her best alone is the one worth
    // the most (CGambits::BestWeaponSkill)
    auto wsWorth(CCharEntity* PMember, uint16 skill, CBattleEntity* PMob) -> double;

    // What the party's cardians say of the plan, in the party's chat
    // (skillchain_math.h kAskLines, kSaveLines). With no plan picked and the
    // party able to make a skillchain, one of the player's cardians asks
    // once a session, settled into one of its first fights (AskWatch); a
    // member of the plan keeping her TP for the next fight says so once a
    // fight. Asked by her weapon skill row; a played character never speaks
    void askIfDue(CCharEntity* PSpeaker, CBattleEntity* PMob);
    void saySaving(CCharEntity* PMember, CBattleEntity* PMob);

    // The plan's calls in the party's chat, as a player's macros make them
    // (skillchain_math.h openingLine, readyLine), once a fight each: its
    // opener's as she opens it -- or the moment he calls Skillchain now --
    // and its closer's once she has the TP for her close, in every fight
    // while she holds it. A played character never speaks. The call sounds
    // are his (PREFS), none until his addon sends them
    void sayOpening(CCharEntity* PMember, CBattleEntity* PMob);
    void sayReady(CCharEntity* PMember, CBattleEntity* PMob);
    void setCalls(uint32 playerCharID, uint8 opening, uint8 ready);
} // namespace pawn::chains
