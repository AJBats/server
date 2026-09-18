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
#include <cstdlib>
#include <chrono>
#include <map>
#include <optional>
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
        // Attendance is independent of casts/hits. Capture it at opening too,
        // so a fight ending before the next shared tick still counts.
        for (const auto id : m_members)
        {
            if (auto* member = zoneutils::GetChar(id); member != nullptr)
            {
                r.attend(id, member->getName(), static_cast<uint16>(member->getZone()));
            }
        }
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

    void FightLog::vanished(const uint32 casterId, const Pending& p, CBattleEntity* PCaster)
    {
        // Booked on the fight the cast began in, which may be settling by
        // now, else the fight on its target
        FightRecord* r = nullptr;
        if (const auto i = p.record != 0 ? openIndex(p.record) : m_open.size(); i < m_open.size())
        {
            r = &m_open[i];
        }
        if (r == nullptr)
        {
            r = recordForTarget(p.target != 0 ? p.target : casterId);
        }
        if (r != nullptr)
        {
            ++r->member(casterId, PCaster->getName()).interrupted;
        }
        if (!debug())
        {
            return;
        }
        auto*          PSpell  = spell::GetSpell(static_cast<SpellID>(p.spell));
        CBattleEntity* PTarget = nullptr;
        if (p.target != 0)
        {
            auto* PEntity = zoneutils::GetEntity(p.target);
            PTarget       = PEntity != nullptr && PEntity->id == p.target ? dynamic_cast<CBattleEntity*>(PEntity) : nullptr;
        }
        // What the log can tell now, said as now: the server keeps the reason
        std::string why;
        if (PCaster->isDead())
        {
            why = "she is down";
        }
        else if (p.target != 0 && (PTarget == nullptr || PTarget->isDead()))
        {
            why = "the target is gone";
        }
        else if (PTarget != nullptr && PSpell != nullptr && PTarget != PCaster)
        {
            const float reach = PSpell->getRange() + PCaster->modelHitboxSize + PTarget->modelHitboxSize;
            const float away  = distance(PCaster->loc.p, PTarget->loc.p);
            if (away > reach)
            {
                why = fmt::format("{} is {:.1f} y away now, the reach {:.1f}", PTarget->getName(), away, reach);
            }
        }
        if (why.empty())
        {
            const float moved = distance(p.at, PCaster->loc.p, true);
            why               = moved > 0.3f ? fmt::format("she is {:.1f} y from where the cast began", moved) : std::string("no reason the server gave (paralysed, or refused as it began)");
        }
        ShowInfoFmt("tactics: {}'s {} on {} ended without landing: {}", PCaster->getName(), PSpell != nullptr ? PSpell->getName() : "cast",
                    PTarget != nullptr ? PTarget->getName() : "?", why);
    }

    void FightLog::tick(const timer::time_point now, const std::vector<CBattleEntity*>& members)
    {
        const double nowSecs = seconds(now);
        const bool   anyone  = !members.empty();

        // A cast the log saw start that vanished without landing and without
        // an interruption event: the server fires MAGIC_INTERRUPTED only for
        // a caster slept or stunned at the end of the cast; out of range,
        // moved, paralysed and a lost target end a cast in silence. Booked
        // once the cast should have ended (a younger one is suspended under
        // a stun, not over). A caster no longer in the scope is forgotten
        for (auto it = m_pending.begin(); it != m_pending.end();)
        {
            CBattleEntity* PCaster = nullptr;
            for (auto* PMember : members)
            {
                if (PMember != nullptr && PMember->id == it->first)
                {
                    PCaster = PMember;
                    break;
                }
            }
            if (PCaster == nullptr)
            {
                it = m_pending.erase(it);
                continue;
            }
            if (PCaster->PAI->IsCurrentState<CMagicState>())
            {
                ++it;
                continue;
            }
            const auto*  PSpell   = spell::GetSpell(static_cast<SpellID>(it->second.spell));
            const double castTime = PSpell != nullptr ? std::chrono::duration<double>(PSpell->getCastTime()).count() : 0.0;
            if (nowSecs - it->second.startedAt < castTime)
            {
                ++it;
                continue;
            }
            vanished(it->first, it->second, PCaster);
            it = m_pending.erase(it);
        }

        const bool anyUp = std::any_of(members.begin(), members.end(), [](const CBattleEntity* PMember)
                                       {
                                           return !PMember->isDead();
                                       });
        for (std::size_t i = 0; i < m_open.size();)
        {
            auto&       r    = m_open[i];
            for (auto* member : members)
            {
                if (member != nullptr)
                {
                    r.attend(member->id, member->getName(), static_cast<uint16>(member->getZone()));
                }
            }
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
                // The bank's price list, one member a tick as the fight
                // opens: every debuff she could cast on this mob
                for (auto* PMember : members)
                {
                    if (PMember != nullptr && PMember->objtype == TYPE_PC && !r.priced.contains(PMember->id))
                    {
                        r.priced.insert(PMember->id);
                        for (const auto& line : bank::priceMember(r, spotAverages(r.zone, r.mobName), exchange(), members, PMember, PMob))
                        {
                            r.priceList.push_back(line);
                            ShowInfoFmt("tactics: {}", line);
                        }
                        break;
                    }
                }
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

    void FightLog::onMagicStart(CBattleEntity* PCaster, CBattleEntity* PTarget, CSpell* PSpell)
    {
        if (PCaster != nullptr)
        {
            auto pending = Pending{ .target = PTarget != nullptr ? PTarget->id : 0, .hp = PTarget != nullptr ? PTarget->health.hp : 0, .maxHp = PTarget != nullptr ? PTarget->GetMaxHP() : 0 };
            if (PSpell != nullptr && PSpell->isCure())
            {
                pending.expected = bank::expectedCure(PCaster, PSpell, PTarget).value_or(-1);
            }
            pending.spell     = PSpell != nullptr ? static_cast<uint16>(PSpell->getID()) : 0;
            pending.at        = PCaster->loc.p;
            pending.startedAt = seconds(timer::now());
            if (pending.target != 0 && openIndex(pending.target) < m_open.size())
            {
                pending.record = pending.target;
            }
            else if (const auto* r = recordForTarget(pending.target != 0 ? pending.target : PCaster->id); r != nullptr)
            {
                pending.record = r->mobId;
            }
            // A cast still pending as the next begins ended without a word:
            // booked before it is forgotten
            if (const auto it = m_pending.find(PCaster->id); it != m_pending.end())
            {
                vanished(it->first, it->second, PCaster);
                m_pending.erase(it);
            }
            m_pending[PCaster->id] = pending;
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
            target = it->second.target != 0 ? it->second.target : target;
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
        // The gap a cure faced, as the caster decided: the target's HP when
        // the cast started, else rebuilt from what landed
        std::optional<Pending> started;
        if (const auto it = m_pending.find(PCaster->id); it != m_pending.end())
        {
            started = it->second;
            m_pending.erase(it);
        }
        const int32 mp = battleutils::CalculateSpellCost(PCaster, PSpell);

        CastNote     note{ .caster = PCaster->id, .target = PTarget != nullptr ? PTarget->id : 0, .spell = static_cast<uint16>(PSpell->getID()), .spellName = PSpell->getName(), .mp = mp };
        int32        overcure = 0;
        int32        missing  = 0;
        FightRecord* r        = nullptr;

        if (PSpell->isCure() && PTarget != nullptr)
        {
            // Every target's HP counts against the cost (a Curaga heals the
            // party); the primary target alone is the estimate's sample
            const int32 primary = PLuaAction != nullptr ? PLuaAction->getParam(PTarget->id) : 0;
            note.cure           = true;
            missing             = started && started->target == PTarget->id ? started->maxHp - started->hp : PTarget->GetMaxHP() - std::max(0, PTarget->health.hp - primary);
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
            auto& memory  = cureMemory(PCaster->id, PSpell);
            ++memory.tally.casts;
            memory.tally.mpSpent += mp;
            memory.tally.hpLanded += note.landed;
            if (note.toppedUp)
            {
                ++memory.tally.toppedUp;
            }
            memory.estimate.note(primary, note.toppedUp);
            // The formula against the cast (the user's acceptance test):
            // an uncapped cure lands what the sampler said, give or take
            // the day and weather roll, ten percent either way
            if (!note.toppedUp && started && started->expected >= 0)
            {
                if (std::abs(started->expected - primary) > started->expected / 10 + 1)
                {
                    ShowInfoFmt("tactics: bank: cure drift: {}'s {} landed {}, the formula said {}", PCaster->getName(), PSpell->getName(), primary, started->expected);
                }
                else if (tactics::debug())
                {
                    ShowInfoFmt("tactics: bank: {}'s {} landed {}, as the formula said", PCaster->getName(), PSpell->getName(), primary);
                }
            }
            overcure = note.toppedUp ? std::max<int32>(0, memory.estimate.predict() - primary) : 0;
            r        = recordForTarget(PTarget->id);
            m_cureHp += note.landed;
            m_cureMp += mp;
            ++m_cureCasts;
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
            bankLine(nullptr, PCaster, PTarget, PSpell, missing);
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
        bankLine(r, PCaster, PTarget, PSpell, missing);
    }

    // What the bank makes of a cast: the cure's tier table and its pick
    // (a cure between fights included), a debuff's price, or unpriced with
    // its family
    void FightLog::bankLine(FightRecord* r, CBattleEntity* PCaster, CBattleEntity* PTarget, CSpell* PSpell, const int32 missing)
    {
        if (const auto line = bank::castLine(r, exchange(), PCaster, PTarget, PSpell, missing); !line.empty())
        {
            ShowInfoFmt("tactics: {}", line);
        }
    }

    auto FightLog::exchange() const -> cardian::tactics::Exchange
    {
        return cardian::tactics::exchange(m_cureHp, m_cureMp, m_cureCasts);
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

    void FightLog::onMobDamaged(CMobEntity* PMob, const int32 amount, CBattleEntity* PAttacker, const xi::AttackType attackType)
    {
        if (PMob == nullptr || amount <= 0)
        {
            return;
        }
        auto&       r      = recordFor(PMob, false);
        const int32 landed = std::min(amount, std::max<int32>(PMob->health.hp, 0));
        r.mobDamage += landed;
        if (PAttacker == nullptr)
        {
            // A regen tick is one damage call from nobody for the regen the
            // mob is losing, or less under stoneskin: the debuffs' own. A
            // weapon's added effect arrives from nobody too, and is not
            // theirs
            if (attackType == xi::AttackType::None && amount <= PMob->getMod(xi::Mod::REGEN_DOWN))
            {
                bank::dotSplit(r, PMob, landed);
            }
            return;
        }
        if (!isMember(PAttacker->id))
        {
            return; // somebody not ours
        }
        auto&      m          = r.member(PAttacker->id, PAttacker->getName());
        const bool weaponSkill = PAttacker->PAI->IsCurrentState<CWeaponSkillState>();
        m.damageDealt += landed;
        if (weaponSkill)
        {
            m.wsDamage += landed;
            ++m.wsCount;
        }
        if (attackType == xi::AttackType::Physical && !weaponSkill)
        {
            // A melee swing met the mob's defence; magic, ranged, skillchains
            // and weapon skills (their own attack and defence terms) are not
            // priced on it
            bank::defenceSplit(r, PAttacker, PMob, landed);
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

    void FightLog::onMemberParalyzed(CBattleEntity* PMember)
    {
        if (PMember == nullptr)
        {
            return;
        }
        // Hers only if she is in that fight already; a proc between fights
        // belongs to nobody's record
        if (auto* r = recordForTarget(PMember->id); r != nullptr && r->find(PMember->id) != nullptr)
        {
            ++r->member(PMember->id, PMember->getName()).paralysed;
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} is paralysed", PMember->getName());
        }
    }

    auto FightLog::priceLists() const -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& r : m_open)
        {
            out.insert(out.end(), r.priceList.begin(), r.priceList.end());
        }
        return out;
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

    auto cureMemory(const uint32 caster, CSpell* PSpell) -> CureMemory&
    {
        auto& memory = cures[{ caster, static_cast<uint16>(PSpell->getID()) }];
        if (memory.spell.empty())
        {
            memory.spell          = PSpell->getName();
            memory.estimate.floor = minimumCure(PSpell);
        }
        return memory;
    }

    auto cureEstimate(const uint32 caster, CSpell* PSpell) -> std::pair<int32, bool>
    {
        if (const auto it = cures.find({ caster, static_cast<uint16>(PSpell->getID()) }); it != cures.end())
        {
            return { it->second.estimate.predict(), it->second.estimate.known() };
        }
        return { minimumCure(PSpell), false };
    }

    auto cureLines(const uint32 caster, const std::string_view name) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& [key, memory] : cures)
        {
            if (key.first != caster || memory.tally.casts == 0)
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
