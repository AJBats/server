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

#include "fight_log.h"

#include "tactics.h"

#include "common/logging.h"

#include "action/action.h"
#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "ai/states/mobskill_state.h"
#include "ai/states/weaponskill_state.h"
#include "entities/battle_entity.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "lua/lua_action.h"
#include "lua/lua_spell.h"
#include "mobskill.h"
#include "spell.h"
#include "utils/battleutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <tuple>
#include <utility>

namespace pawn::tactics
{
    namespace
    {
        using cardian::tactics::CastNote;

        // The tier's minimum cure, the floor of the estimate before she has
        // shown her own (scripts/actions/spells/white/cure*.lua)
        auto minimumCure(CSpell* PSpell) -> int32
        {
            switch (PSpell->getID())
            {
                case SpellID::Cure:
                    return 10;
                case SpellID::Cure_II:
                case SpellID::Curaga:
                    return 60;
                case SpellID::Cure_III:
                case SpellID::Curaga_II:
                    return 130;
                case SpellID::Cure_IV:
                case SpellID::Curaga_III:
                    return 270;
                case SpellID::Cure_V:
                    return 450;
                case SpellID::Cure_VI:
                    return 600;
                default:
                    return 0;
            }
        }

        // The memories
        std::map<std::pair<uint16, std::string>, SpotAverages>               spots;
        std::map<std::pair<uint32, uint16>, CureMemory>                      cures;
        std::map<std::tuple<uint16, std::string, uint16>, DebuffMemory>      debuffs;
        std::map<std::pair<uint16, std::string>, ProcValue>                  procs;
    } // namespace

    auto seconds(const timer::time_point tp) -> double
    {
        return std::chrono::duration<double>(tp.time_since_epoch()).count();
    }

    auto asMob(CBattleEntity* PEntity) -> CMobEntity*
    {
        return PEntity != nullptr && PEntity->objtype == TYPE_MOB ? static_cast<CMobEntity*>(PEntity) : nullptr;
    }

    FightLog::~FightLog()
    {
        const auto now = timer::now();
        while (!m_open.empty())
        {
            close(0, "scope ended", now);
        }
    }

    void FightLog::addMember(const uint32 id)
    {
        m_members.insert(id);
    }

    void FightLog::removeMember(const uint32 id)
    {
        m_members.erase(id);
    }

    auto FightLog::isMember(const uint32 id) const -> bool
    {
        return m_members.contains(id);
    }

    auto FightLog::hasOpen(const uint32 mobId) const -> bool
    {
        return openIndex(mobId) < m_open.size();
    }

    auto FightLog::openIndex(const uint32 mobId) const -> std::size_t
    {
        for (std::size_t i = 0; i < m_open.size(); ++i)
        {
            if (m_open[i].mobId == mobId)
            {
                return i;
            }
        }
        return m_open.size();
    }

    auto FightLog::recordFor(CMobEntity* PMob, const bool hitchIt) -> FightRecord&
    {
        if (const auto i = openIndex(PMob->id); i < m_open.size())
        {
            return m_open[i];
        }
        FightRecord r;
        r.mobId    = PMob->id;
        r.mobName  = PMob->getName();
        r.zone     = PMob->loc.zone != nullptr ? static_cast<uint16>(PMob->loc.zone->GetID()) : 0;
        r.zoneName = PMob->loc.zone != nullptr ? PMob->loc.zone->getName() : "";
        r.mobMaxHp = PMob->GetMaxHP();
        r.openedAt = seconds(timer::now());
        // What it lost before we watched: the opening blow of a ranged or
        // magic pull lands before any event of ours reaches the log
        r.mobDamage = std::max<int32>(0, r.mobMaxHp - PMob->health.hp);
        // Two mobs at once are two records, each marked as a link; a fight
        // still settling after its kill is not company
        for (auto& other : m_open)
        {
            if (!other.settling())
            {
                other.overlapping = true;
                r.overlapping     = true;
            }
        }
        if (hitchIt)
        {
            hitch(PMob);
        }
        if (debug())
        {
            ShowInfoFmt("tactics: fight opens with {} ({} HP{}){}", r.mobName, r.mobMaxHp, r.mobDamage > 0 ? fmt::format(", {} already lost", r.mobDamage) : "", r.overlapping ? ", linked" : "");
        }
        m_open.push_back(std::move(r));
        return m_open.back();
    }

    auto FightLog::recordForTarget(const uint32 targetId) -> FightRecord*
    {
        auto pick = [&](const bool settling) -> FightRecord*
        {
            for (auto& r : m_open)
            {
                if (r.settling() == settling && r.hitting == targetId)
                {
                    return &r;
                }
            }
            for (auto& r : m_open)
            {
                if (r.settling() == settling)
                {
                    return &r;
                }
            }
            return nullptr;
        };
        if (auto* live = pick(false); live != nullptr)
        {
            return live;
        }
        return pick(true);
    }

    void FightLog::close(const std::size_t index, std::string why, const timer::time_point now)
    {
        FightRecord r = std::move(m_open[index]);
        m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
        r.closedAt = r.diedAt > 0.0 ? r.diedAt : seconds(now);
        r.closeWhy = std::move(why);
        ShowInfoFmt("{}", cardian::tactics::summary(r));
        spotAverages(r.zone, r.mobName).fold(r);
        m_recent.push_front(std::move(r));
        while (m_recent.size() > 10)
        {
            m_recent.pop_back();
        }
        ++m_closed;
    }

    void FightLog::tick(const timer::time_point now, const std::vector<CBattleEntity*>& members)
    {
        const double nowSecs = seconds(now);
        const bool   anyone  = !members.empty();
        const bool   anyUp   = std::any_of(members.begin(), members.end(), [](const CBattleEntity* PMember)
                                           {
                                               return !PMember->isDead();
                                           });
        for (std::size_t i = 0; i < m_open.size();)
        {
            auto&       r    = m_open[i];
            auto*       PMob = asMob(dynamic_cast<CBattleEntity*>(zoneutils::GetEntity(r.mobId, TYPE_MOB)));
            std::string why;
            if (PMob == nullptr)
            {
                why = "gone"; // despawned
            }
            else if (r.settling())
            {
                if (nowSecs - r.settlingSince > 10.0)
                {
                    why = r.closeWhy.empty() ? "killed" : r.closeWhy;
                }
            }
            else if (PMob->isDead())
            {
                // Dead to the eye before its death event fires: the event
                // closes it as killed; five seconds without one is gone
                if (r.deadSince == 0.0)
                {
                    r.deadSince = nowSecs;
                }
                else if (nowSecs - r.deadSince > 5.0)
                {
                    r.diedAt = r.deadSince;
                    why      = "gone";
                }
            }
            else if (anyone && !anyUp)
            {
                why = "wipe";
            }
            else
            {
                const bool onUs = std::any_of(members.begin(), members.end(), [&](CBattleEntity* PMember)
                                              {
                                                  return PMember->GetBattleTarget() == PMob || PMob->GetBattleTarget() == PMember;
                                              });
                if (onUs)
                {
                    r.idleSince = 0.0;
                }
                else if (r.idleSince == 0.0)
                {
                    r.idleSince = nowSecs;
                }
                else if (nowSecs - r.idleSince > 5.0)
                {
                    why         = "left";
                    r.mobHpLeft = PMob->health.hp;
                }
            }
            if (!why.empty())
            {
                close(i, std::move(why), now);
            }
            else
            {
                ++i;
            }
        }
    }

    // --- member events --------------------------------------------------

    void FightLog::onMemberDamaged(CBattleEntity* PMember, const int32 amount, CBattleEntity* PAttacker)
    {
        auto* PMob = asMob(PAttacker);
        if (PMember == nullptr || PMob == nullptr || amount <= 0)
        {
            return;
        }
        auto&       r      = recordFor(PMob, true);
        auto&       m      = r.member(PMember->id, PMember->getName());
        const int32 landed = std::min(amount, std::max<int32>(PMember->health.hp, 0)); // the listener fires before her HP moves
        const bool  tpMove = PMob->PAI->IsCurrentState<CMobSkillState>();
        m.damageTaken += landed;
        m.biggestHit = std::max(m.biggestHit, amount);
        ++m.hits;
        if (tpMove)
        {
            m.tpMoveDamage += landed;
            if (!r.tpMoveNames.empty())
            {
                r.tpMoveNames.back().second += landed;
            }
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} takes {} from {}{} ({} HP left)", m.name, amount, r.mobName, tpMove ? "'s TP move" : "", PMember->health.hp - landed);
        }
    }

    void FightLog::onMagicStart(CBattleEntity* PCaster, CBattleEntity* PTarget)
    {
        if (PCaster != nullptr)
        {
            m_pending[PCaster->id] = PTarget != nullptr ? PTarget->id : 0;
        }
    }

    void FightLog::onMagicInterrupted(CBattleEntity* PCaster)
    {
        if (PCaster == nullptr)
        {
            return;
        }
        uint32 target = PCaster->id;
        if (const auto it = m_pending.find(PCaster->id); it != m_pending.end())
        {
            target = it->second != 0 ? it->second : target;
            m_pending.erase(it);
        }
        if (auto* r = recordForTarget(target); r != nullptr)
        {
            ++r->member(PCaster->id, PCaster->getName()).interrupted;
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {}'s cast is interrupted", PCaster->getName());
        }
    }

    void FightLog::onMagicUse(CBattleEntity* PCaster, CBattleEntity* PTarget, CLuaSpell* PLuaSpell, CLuaAction* PLuaAction)
    {
        CSpell* PSpell = PLuaSpell != nullptr ? PLuaSpell->GetSpell() : nullptr;
        if (PCaster == nullptr || PSpell == nullptr)
        {
            return;
        }
        m_pending.erase(PCaster->id);
        const int32 mp = battleutils::CalculateSpellCost(PCaster, PSpell);

        CastNote     note{ .caster = PCaster->id, .target = PTarget != nullptr ? PTarget->id : 0, .spell = static_cast<uint16>(PSpell->getID()), .spellName = PSpell->getName(), .mp = mp };
        int32        overcure = 0;
        FightRecord* r        = nullptr;

        if (PSpell->isCure() && PTarget != nullptr)
        {
            // Every target's HP counts against the cost (a Curaga heals the
            // party); the primary target alone is the estimate's sample
            const int32 primary = PLuaAction != nullptr ? PLuaAction->getParam(PTarget->id) : 0;
            note.cure           = true;
            note.landed         = 0;
            if (PLuaAction != nullptr && PLuaAction->GetAction() != nullptr)
            {
                for (const auto& target : PLuaAction->GetAction()->targets)
                {
                    for (const auto& result : target.results)
                    {
                        note.landed += std::max<int32>(0, result.param);
                    }
                }
            }
            note.toppedUp = PTarget->health.hp >= PTarget->GetMaxHP();
            auto& memory  = cureMemory(PCaster->id, note.spell, note.spellName, minimumCure(PSpell));
            ++memory.tally.casts;
            memory.tally.mpSpent += mp;
            memory.tally.hpLanded += note.landed;
            if (note.toppedUp)
            {
                ++memory.tally.toppedUp;
            }
            memory.estimate.note(primary, note.toppedUp);
            overcure = note.toppedUp ? std::max<int32>(0, memory.estimate.predict() - primary) : 0;
            r        = recordForTarget(PTarget->id);
        }
        else if (auto* PMob = asMob(PTarget); PMob != nullptr)
        {
            r = &recordFor(PMob, true);
            if (PSpell->isDebuff())
            {
                note.debuff     = true;
                note.tookEffect = PSpell->tookEffect();
                debuffMemory(r->zone, PMob->getName(), note.spell, note.spellName).value.note(note.tookEffect);
            }
        }
        else
        {
            r = recordForTarget(PTarget != nullptr ? PTarget->id : PCaster->id);
        }

        if (debug())
        {
            ShowInfoFmt("tactics: {} casts {} on {} for {} MP{}{}", PCaster->getName(), note.spellName, PTarget != nullptr ? PTarget->getName() : "nobody", mp,
                        note.cure ? fmt::format(", {} HP landed{}", note.landed, note.toppedUp ? " (topped up)" : "") : "",
                        note.debuff ? (note.tookEffect ? ", landed" : ", resisted") : "");
        }
        if (r == nullptr)
        {
            return;
        }
        auto& m = r->member(PCaster->id, PCaster->getName());
        ++m.casts;
        m.mpSpent += mp;
        if (note.cure)
        {
            m.hpCured += note.landed;
            m.overcure += overcure;
        }
        r->casts.push_back(std::move(note));
    }

    void FightLog::onAttacked(CBattleEntity* PMember, CBattleEntity* PAttacker)
    {
        auto* PMob = asMob(PAttacker);
        if (PMember == nullptr || PMob == nullptr)
        {
            return;
        }
        auto& r = recordFor(PMob, true);
        if (r.hitting != PMember->id)
        {
            if (r.hitting != 0)
            {
                ++r.switches;
                if (debug())
                {
                    ShowInfoFmt("tactics: {} turns onto {}", r.mobName, PMember->getName());
                }
            }
            r.hitting = PMember->id;
            ++r.member(PMember->id, PMember->getName()).targeted;
        }
    }

    void FightLog::onEngage(CBattleEntity* PMember, CBattleEntity* PTarget)
    {
        if (auto* PMob = asMob(PTarget); PMember != nullptr && PMob != nullptr)
        {
            recordFor(PMob, true).member(PMember->id, PMember->getName());
        }
    }

    void FightLog::onMemberDeath(CBattleEntity* PMember, CBattleEntity* PKiller)
    {
        if (PMember == nullptr)
        {
            return;
        }
        FightRecord* r = nullptr;
        if (auto* PMob = asMob(PKiller); PMob != nullptr && hasOpen(PMob->id))
        {
            r = &m_open[openIndex(PMob->id)];
        }
        else
        {
            r = recordForTarget(PMember->id);
        }
        if (r != nullptr)
        {
            ++r->member(PMember->id, PMember->getName()).deaths;
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} is down{}", PMember->getName(), PKiller != nullptr ? fmt::format(", killed by {}", PKiller->getName()) : "");
        }
    }

    // --- mob events -----------------------------------------------------

    void FightLog::onMobDamaged(CMobEntity* PMob, const int32 amount, CBattleEntity* PAttacker)
    {
        if (PMob == nullptr || amount <= 0)
        {
            return;
        }
        auto&       r      = recordFor(PMob, false);
        const int32 landed = std::min(amount, std::max<int32>(PMob->health.hp, 0));
        r.mobDamage += landed;
        if (PAttacker == nullptr || !isMember(PAttacker->id))
        {
            return; // a damage-over-time tick, or somebody not ours
        }
        auto& m = r.member(PAttacker->id, PAttacker->getName());
        m.damageDealt += landed;
        if (PAttacker->PAI->IsCurrentState<CWeaponSkillState>())
        {
            m.wsDamage += landed;
            ++m.wsCount;
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} deals {} to {} ({} HP left, {}%)", m.name, landed, r.mobName, PMob->health.hp - landed, r.mobMaxHp > 0 ? (PMob->health.hp - landed) * 100 / r.mobMaxHp : 0);
        }
    }

    void FightLog::onMobDeath(CMobEntity* PMob)
    {
        if (PMob == nullptr)
        {
            return;
        }
        if (const auto i = openIndex(PMob->id); i < m_open.size() && !m_open[i].settling())
        {
            // Won; the cures that follow the kill still count for ten
            // seconds, then the record closes on its tick
            auto& r         = m_open[i];
            r.diedAt        = seconds(timer::now());
            r.settlingSince = r.diedAt;
            r.closeWhy      = "killed";
            if (debug())
            {
                ShowInfoFmt("tactics: {} is dead; the fight settles", r.mobName);
            }
        }
    }

    void FightLog::onMobTpMove(CMobEntity* PMob, const uint16 skillId)
    {
        if (PMob == nullptr)
        {
            return;
        }
        if (const auto i = openIndex(PMob->id); i < m_open.size())
        {
            auto&      r      = m_open[i];
            auto*      PSkill = battleutils::GetMobSkill(skillId);
            const auto name   = PSkill != nullptr ? PSkill->getName() : fmt::format("skill {}", skillId);
            r.tpMoveNames.emplace_back(name, 0);
            if (debug())
            {
                ShowInfoFmt("tactics: {} readies {}", r.mobName, name);
            }
        }
    }

    void FightLog::onMobParalyzed(CMobEntity* PMob)
    {
        if (PMob == nullptr)
        {
            return;
        }
        const auto i = openIndex(PMob->id);
        if (i >= m_open.size())
        {
            return;
        }
        auto& r = m_open[i];
        ++r.procs;
        auto& value = procValue(r.zone, r.mobName);
        if (PMob->PAI->IsCurrentState<CMagicState>())
        {
            ++value.spells; // an interrupt avoided: unpriced yet
        }
        else
        {
            ++value.melee;
            if (const auto* target = r.find(r.hitting); target != nullptr)
            {
                value.hpStopped += target->meanHit();
            }
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} is paralyzed{}", r.mobName, PMob->PAI->IsCurrentState<CMagicState>() ? " mid-cast" : "");
        }
    }

    // --- the memories ---------------------------------------------------

    auto spotAverages(const uint16 zone, const std::string& mob) -> SpotAverages&
    {
        return spots[{ zone, mob }];
    }

    auto spotLines(const uint16 zone) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& [key, spot] : spots)
        {
            if (key.first == zone)
            {
                out.push_back(spot.line(key.second));
            }
        }
        return out;
    }

    auto cureMemory(const uint32 caster, const uint16 spell, const std::string_view name, const int32 minimumCure) -> CureMemory&
    {
        auto& memory = cures[{ caster, spell }];
        if (memory.spell.empty())
        {
            memory.spell          = std::string(name);
            memory.estimate.floor = minimumCure;
        }
        return memory;
    }

    auto cureLines(const uint32 caster, const std::string_view name) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& [key, memory] : cures)
        {
            if (key.first != caster)
            {
                continue;
            }
            const auto& t = memory.tally;
            out.push_back(fmt::format("{} {} x{}: {} MP -> {} HP ({:.1f} HP/MP), heals for {}{}{}", name, memory.spell, t.casts, t.mpSpent, t.hpLanded, t.hpPerMp(),
                                      memory.estimate.predict(), memory.estimate.known() ? "" : " at least",
                                      t.toppedUp > 0 ? fmt::format(", {} topped up", t.toppedUp) : ""));
        }
        return out;
    }

    auto debuffMemory(const uint16 zone, const std::string& mob, const uint16 spell, const std::string_view name) -> DebuffMemory&
    {
        auto& memory = debuffs[{ zone, mob, spell }];
        if (memory.spell.empty())
        {
            memory.spell = std::string(name);
        }
        return memory;
    }

    auto procValue(const uint16 zone, const std::string& mob) -> ProcValue&
    {
        return procs[{ zone, mob }];
    }

    auto debuffLines(const uint16 zone) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& [key, memory] : debuffs)
        {
            if (std::get<0>(key) != zone)
            {
                continue;
            }
            const auto& v = memory.value;
            out.push_back(fmt::format("{} on {}: {} cast, {} landed, {} resisted", memory.spell, std::get<1>(key), v.casts, v.lands, v.resists));
        }
        for (const auto& [key, value] : procs)
        {
            if (key.first == zone)
            {
                out.push_back(fmt::format("paralysis procs on {}: {} on swings, {} on spells, ~{:.0f} HP stopped", key.second, value.melee, value.spells, value.hpStopped));
            }
        }
        return out;
    }
} // namespace pawn::tactics
