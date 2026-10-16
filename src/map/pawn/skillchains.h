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
// on a mob, and the Black Mage's telepathy -- when the chain lands. The
// game's own rules are asked of the server (battleutils), never copied.
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
    // the session, as his strategy is
    struct Plan
    {
        uint32 opener      = 0;
        uint16 openerSkill = 0;
        uint32 closer      = 0;
        uint16 closerSkill = 0;
        uint8  chain       = 0;
    };
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

    // A member out of a party: a plan naming her ends, and so does a
    // player's own as he leaves (pawn::leftParty)
    void memberLeft(const CBattleEntity* PMember);

    // A member's place in the plan of the party she fights for, on her mob:
    // her seat and her weapon skill in it, the chain, and her partner --
    // whether he fights the same mob and follows the plan (a cardian's
    // tactician's Weapon skill (best) runs, her gambits on; the plan's
    // weapon skill his now), and with the TP for his weapon skill. Every
    // member, in it or not, knows what the plan's opening looks like and
    // whether its closer plays his part on her mob
    struct Part
    {
        cardian::chains::Seat       seat          = cardian::chains::Seat::None;
        uint16                      skill         = 0;
        uint8                       chain         = 0;
        bool                        partnerFights = false;
        bool                        partnerReady  = false;
        std::string                 partner;
        cardian::chains::Properties planOpening{};
        bool                        planCloserFights = false;
        std::string                 planCloser;
    };
    auto partOf(CCharEntity* PMember, CBattleEntity* PMob) -> Part;

    // Telepathy: the chain open on a mob, or expected on it, as the Black
    // Mage's nukes read it -- a plan's only while both its members play
    // their part on the mob, not on a mob under half its HP before the
    // opening, and not once both have had the TP for longer than the
    // opening takes with none come. `open`: a burst window open now and the seconds
    // it has left; `in`: a chain expected in this many seconds; `count`: the
    // chain's links, which the burst's damage grows with; `how`: how it was
    // read, for the map log. Every member asked about has her TP sampled
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
    // it; or, for a chain, a member of the party in the zone can burst its
    // elements
    auto othersClose(CCharEntity* PSelf, CBattleEntity* PMob, const cardian::chains::Resonance& r) -> bool;
    auto burstComing(CCharEntity* PSelf, const cardian::chains::Resonance& r) -> bool;

    // A member's TP, sampled for her rate (skillchain_math.h TpClock); every
    // gambit engine's tick samples its own, and the forecast those it asks about
    void noteTp(CBattleEntity* PMember);
} // namespace pawn::chains
