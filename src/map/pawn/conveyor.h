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

#include "conveyor_math.h"
#include "spell_bank.h"

#include "common/cbasetypes.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CBattleEntity;
class CCharEntity;
class CSpell;

namespace pawn::tactics
{
    using cardian::tactics::Need;
    using cardian::tactics::NeedKey;
    using cardian::tactics::NeedKind;
    using cardian::tactics::Request;
    using cardian::tactics::Source;

    // The conveyor (RESEARCH §12.12 item 2, §12.14), the tactician's
    // referee: rows and the role feed requests, requests merge into needs,
    // a cast in flight is a locked need whether or not anyone fed it (a
    // real player's too), and each need is assigned one caster, who reads
    // it in her slot's order. It decides who; the caster's controller does
    // the casting. Nothing here outlives a call: the scope's members and
    // role holders are handed in fresh each time, derived from the party
    // pointers the game holds.
    class Conveyor
    {
    public:
        struct Scope
        {
            std::vector<CBattleEntity*> members; // the alliance's characters, the player included
            std::unordered_set<uint32>  holders; // those whose Role row says Support Mage
        };

        // A need's key from what would be cast: a cure on a member, a
        // status spell by its family on its target, damage never merging.
        // A self-only spell is keyed on the caster, where the cast will go
        static auto keyFor(CSpell* PSpell, uint32 target, uint32 caster) -> NeedKey;

        // A member or a mob by id: the scope's members first, then the
        // zone-coded mob lookup checked against the id. Never a walk of
        // every zone, and never a stranger for a character's id
        static auto resolve(const Scope& scope, uint32 id) -> CBattleEntity*;

        // A request, merged into its need and, when the need is not locked,
        // assigned at once -- so a row learns on the spot whether the cast
        // is hers and casts it in the same think, as it always did
        auto feed(const NeedKey& key, Request r, const Scope& scope) -> const Need&;

        // Once a tick: the casts in flight read off the members' magic
        // states (each its own locked need), stale requests withdrawn,
        // every unlocked need assigned afresh
        void tick(double now, double life, const Scope& scope);

        // A cast starting (the log's MAGIC_START, the spell from the event):
        // its need opened if nobody fed it, locked from this instant
        void castStarted(CCharEntity* PCaster, CSpell* PSpell, uint32 target);

        // A cast resolved (MAGIC_USE) or interrupted: the need is met, or
        // not, and either way forgotten -- the rows re-feed if their
        // conditions still hold. A cast that landed starts its rows' retry
        // clocks. The spell may be null on an interruption
        void castEnded(CCharEntity* PCaster, CSpell* PSpell, uint32 target, bool landed);

        // Her next cast: the first need in her slot's order that is hers.
        // Not engaged, nothing offensive: a first cast on a mob is a pull
        struct Assignment
        {
            SpellID     spell{};
            uint32      target = 0;
            std::string why;
            bool        approach = false; // a row may walk into range
        };
        auto assignment(uint32 caster, bool engaged, const Scope& scope) const -> std::optional<Assignment>;

        auto needs() const -> const std::vector<Need>&
        {
            return m_needs.needs;
        }

        // For !tactics: each need, who fed it, who has it
        auto lines(const Scope& scope) const -> std::vector<std::string>;

    private:
        void schedule(Need& n, const Scope& scope, std::unordered_map<uint32, uint32>& loads);
        auto spellFor(const Need& n, CCharEntity* PCaster, CBattleEntity* PTarget) -> SpellID; // what she would cast for it; 0 when nothing fits
        auto tiersOf(CCharEntity* PCaster) -> const std::vector<bank::CureTier>&;             // her affordable cure tiers, once a tick
        auto describe(const Need& n, const Scope& scope) const -> std::string;
        static auto offensive(const Need& n, const Scope& scope) -> bool;

        cardian::tactics::Needs                                m_needs;
        std::unordered_map<uint32, NeedKey>                    m_pending; // caster -> the need her cast in flight is for
        std::unordered_map<uint32, std::vector<bank::CureTier>> m_tiers;   // caster -> her tiers this tick
    };
} // namespace pawn::tactics
