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

#include "bank_math.h"
#include "fight_math.h"
#include "spell_bank.h"

#include "common/cbasetypes.h"
#include "common/timer.h"
#include "data/enums/attack_type.h"

#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CBattleEntity;
class CMobEntity;
class CLuaSpell;
class CLuaAction;
class CSpell;

namespace pawn::tactics
{
    using cardian::tactics::CureEstimate;
    using cardian::tactics::CureTally;
    using cardian::tactics::DebuffValue;
    using cardian::tactics::FightRecord;
    using cardian::tactics::ProcValue;
    using cardian::tactics::SpotAverages;

    // Seconds on the map's clock: the unit every record's timestamps carry
    auto seconds(timer::time_point tp) -> double;

    // The fight log (RESEARCH §12.5): one record per mob from the first
    // engagement to its death, a wipe or the party walking away -- the
    // party's shared picture of the fight, the player included. It counts
    // what the dispatcher hands it (tactics.cpp routes a hitched entity's
    // events here by id, or drops them) and never decides anything.
    // Measure what happened; calculate what didn't.
    class FightLog
    {
    public:
        FightLog() = default;
        ~FightLog(); // whatever is still open closes as "scope ended": logged and folded, never lost
        FightLog(const FightLog&)            = delete;
        FightLog& operator=(const FightLog&) = delete;

        // The scope's members, kept by the tactician's refresh: whose
        // damage dealt counts, and whose deaths make a wipe
        void addMember(uint32 id);
        void removeMember(uint32 id);
        auto isMember(uint32 id) const -> bool;

        // Once a tick, with the members the refresh saw this tick: a mob
        // dead without its event, a party wiped, a mob walked away from --
        // the record closes, the map log gets its line, the spot's averages
        // fold it in
        void tick(timer::time_point now, const std::vector<CBattleEntity*>& members);

        auto open() const -> const std::vector<FightRecord>&
        {
            return m_open;
        }
        auto recent() const -> const std::deque<FightRecord>&
        {
            return m_recent;
        }
        auto closedCount() const -> uint32
        {
            return m_closed;
        }
        auto hasOpen(uint32 mobId) const -> bool;

        // What happened, as the dispatcher routes it. A record opened from
        // a member's event hitches the mob; one opened from the mob's own
        // event must not: the event proves the hitch is there, and adding
        // a listener from inside its own trigger mutates the vector LSB is
        // walking
        void onMemberDamaged(CBattleEntity* PMember, int32 amount, CBattleEntity* PAttacker);
        void onMagicStart(CBattleEntity* PCaster, CBattleEntity* PTarget);
        void onMagicUse(CBattleEntity* PCaster, CBattleEntity* PTarget, CLuaSpell* PLuaSpell, CLuaAction* PLuaAction);
        void onMagicInterrupted(CBattleEntity* PCaster);
        void onAttacked(CBattleEntity* PMember, CBattleEntity* PAttacker);
        void onEngage(CBattleEntity* PMember, CBattleEntity* PTarget);
        void onMemberDeath(CBattleEntity* PMember, CBattleEntity* PKiller);
        void onMobDamaged(CMobEntity* PMob, int32 amount, CBattleEntity* PAttacker, xi::AttackType attackType);
        void onMobDeath(CMobEntity* PMob);
        void onMobTpMove(CMobEntity* PMob, uint16 skillId);
        void onMobParalyzed(CMobEntity* PMob);
        void onMemberParalyzed(CBattleEntity* PMember);

        // The bank's price list for each open fight, as printed at its open
        auto priceLists() const -> std::vector<std::string>;

        // The scope's exchange rate (RESEARCH §12.13): HP landed per MP spent
        // over the cures this log has seen, Cure II's floor until three exist
        auto exchange() const -> cardian::tactics::Exchange;

    private:
        auto recordFor(CMobEntity* PMob, bool hitchIt) -> FightRecord&;
        auto openIndex(uint32 mobId) const -> std::size_t; // m_open.size() when none
        auto recordForTarget(uint32 targetId) -> FightRecord*; // the fight whose mob is on the target: live first, then settling, then any
        void bankLine(FightRecord* r, CBattleEntity* PCaster, CBattleEntity* PTarget, CSpell* PSpell, int32 missing);
        void close(std::size_t index, std::string why, timer::time_point now);

        std::unordered_set<uint32> m_members;
        std::vector<FightRecord>   m_open;
        std::deque<FightRecord>    m_recent; // the last few, newest first
        uint32                     m_closed = 0;

        // A cast under way: its target, and the target's HP as the caster
        // decided (the gap a cure faced, RESEARCH §12.13)
        struct Pending
        {
            uint32 target = 0;
            int32  hp     = 0;
            int32  maxHp  = 0;
        };
        std::unordered_map<uint32, Pending> m_pending; // by caster

        // The cures this log has seen, for the exchange rate
        int32  m_cureHp    = 0;
        int32  m_cureMp    = 0;
        uint32 m_cureCasts = 0;
    };


    // A battle entity as the mob it is, or null
    auto asMob(CBattleEntity* PEntity) -> CMobEntity*;

    // Memories that outlive any party (§12.12 item 8): what a mob type
    // costs at a spot, what a caster's cure is worth, what a debuff stops.
    // Spots, debuffs and procs are per zone; a caster's cures are hers
    struct CureMemory
    {
        std::string  spell;
        CureTally    tally;
        CureEstimate estimate;
    };
    struct DebuffMemory
    {
        std::string spell;
        DebuffValue value;
    };

    auto spotAverages(uint16 zone, const std::string& mob) -> SpotAverages&;
    auto spotLines(uint16 zone) -> std::vector<std::string>;
    auto cureMemory(uint32 caster, CSpell* PSpell) -> CureMemory&; // created at the tier's minimum cure as its floor
    auto cureEstimate(uint32 caster, CSpell* PSpell) -> std::pair<int32, bool>; // what she heals for, and whether that is exact; the floor when she has never cast it
    auto cureLines(uint32 caster, std::string_view name) -> std::vector<std::string>;
    auto debuffMemory(uint16 zone, const std::string& mob, uint16 spell, std::string_view name) -> DebuffMemory&;
    auto procValue(uint16 zone, const std::string& mob) -> ProcValue&;
    auto debuffLines(uint16 zone) -> std::vector<std::string>;
} // namespace pawn::tactics
