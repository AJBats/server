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

#include "tactics.h"

#include "conveyor.h"
#include "fight_log.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_gambits.h"
#include "role_support.h"

#include "common/logging.h"
#include "common/settings.h"

#include "ai/ai_container.h"
#include "ai/helpers/event_handler.h"
#include "alliance.h"
#include "entities/battle_entity.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "lua/lua_action.h"
#include "lua/lua_base_entity.h"
#include "lua/lua_spell.h"
#include "lua/luautils.h"
#include "party.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

#include <chrono>
#include <memory>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pawn::tactics
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr std::size_t kChatWidth = 110; // what one chat line holds before the client cuts it
        constexpr auto        kGrace     = 30s; // a member missing from the party list this long is gone (zoning pops her for a moment)

        // pawn.TACTICS_REQUEST_LIFE: a request not re-fed this long is withdrawn
        auto requestLife() -> double
        {
            static const double life = settings::get<float>("pawn.TACTICS_REQUEST_LIFE");
            return life;
        }

        // Her scope as the game holds it this instant: the alliance's
        // characters, and which of them hold the role. Built fresh for
        // every call; nothing keeps an entity pointer across ticks
        auto scopeOf(CCharEntity* PChar) -> Conveyor::Scope
        {
            Conveyor::Scope scope;
            PChar->ForAlliance([&](CBattleEntity* PMember)
                               {
                                   if (PMember != nullptr && PMember->objtype == TYPE_PC)
                                   {
                                       scope.members.push_back(PMember);
                                       if (supportMage(PMember))
                                       {
                                           scope.holders.insert(PMember->id);
                                       }
                                   }
                               });
            return scope;
        }

        struct RosterEntry
        {
            std::string       name;
            uint8             job   = 0;
            uint8             level = 0;
            timer::time_point seenAt{};
        };

        class Tactician;

        // The registry and the routing table, one object leaked on purpose:
        // a tactician's destructor writes the routes, and at exit neither
        // may outlive the other
        struct State
        {
            std::unordered_map<const void*, std::unique_ptr<Tactician>> tacticians; // by scope key
            std::unordered_map<uint32, Tactician*>                      routes;     // member id -> her tactician, rewritten by every scope refresh
            timer::time_point                                           lastSweep{};
            std::vector<CBattleEntity*>                                 scratch; // the members seen this refresh, reused

            // The plumbing's own count since boot, for !tactics: who has
            // been hitched, and how many events the dispatcher kept and
            // dropped
            std::unordered_set<uint32> hitchedMembers;
            std::unordered_set<uint32> hitchedMobs;
            uint64                     routed  = 0;
            uint64                     dropped = 0;
        };
        auto& state = *new State();

        class Tactician
        {
        public:
            explicit Tactician(const uint32 scopeId)
            : m_scopeId(scopeId)
            {
            }

            ~Tactician()
            {
                std::erase_if(state.routes, [this](const auto& kv)
                              {
                                  return kv.second == this;
                              });
            }

            auto scopeId() const -> uint32
            {
                return m_scopeId;
            }
            auto lastTick() const -> timer::time_point
            {
                return m_lastTick;
            }
            auto log() -> FightLog&
            {
                return m_log;
            }
            auto roster() const -> const std::unordered_map<uint32, RosterEntry>&
            {
                return m_roster;
            }
            auto conveyor() -> Conveyor&
            {
                return m_conveyor;
            }

            // The guess before the pull is weak until a few fights have
            // closed since the roster, a job or a level last changed
            auto guessWeak() const -> bool
            {
                return m_log.closedCount() - m_weakFrom < 3;
            }

            void tick(const timer::time_point now, CCharEntity* PAsker)
            {
                if (m_lastTick == now)
                {
                    return;
                }
                m_lastTick = now;
                refreshScope(now, PAsker);
                m_log.tick(now, state.scratch);
                const auto scope = scopeOf(PAsker);
                m_conveyor.tick(seconds(now), requestLife(), scope);
                pace(scope);
            }

            // Out of the scope now: her route and her place on the roster
            void drop(const uint32 id)
            {
                const auto it = m_roster.find(id);
                if (it == m_roster.end())
                {
                    return;
                }
                m_log.removeMember(id);
                if (const auto route = state.routes.find(id); route != state.routes.end() && route->second == this)
                {
                    state.routes.erase(route);
                }
                m_roster.erase(it);
                m_pace.erase(id);
                changed(now());
            }

        private:
            static auto now() -> timer::time_point
            {
                return timer::now();
            }

            void changed(const timer::time_point)
            {
                m_weakFrom = m_log.closedCount();
                if (debug())
                {
                    std::string names;
                    for (const auto& [id, entry] : m_roster)
                    {
                        names += fmt::format("{}{} {}{}", names.empty() ? "" : ", ", entry.name, magic_enum::enum_name(static_cast<xi::Job>(entry.job)), entry.level);
                    }
                    ShowInfoFmt("tactics: scope {} is {} ({}); the guess is weak", m_scopeId, m_roster.size() == 1 ? "one" : fmt::format("{} members", m_roster.size()), names);
                }
            }

            // Step 1 of the tick: the members from the alliance, the party
            // or herself, each routed here and hitched on arrival, a job or
            // level change noted, and a member gone from the list dropped
            // after the grace
            void refreshScope(const timer::time_point now, CCharEntity* PAsker)
            {
                auto& members = state.scratch;
                members.clear();
                PAsker->ForAlliance([&](CBattleEntity* PMember)
                                    {
                                        if (PMember != nullptr && PMember->objtype == TYPE_PC)
                                        {
                                            members.push_back(PMember);
                                        }
                                    });

                bool anyChange = false;
                for (auto* PMember : members)
                {
                    const auto job   = static_cast<uint8>(PMember->GetMJob());
                    const auto level = PMember->GetMLevel();
                    if (const auto it = m_roster.find(PMember->id); it == m_roster.end())
                    {
                        m_roster[PMember->id] = RosterEntry{ PMember->getName(), job, level, now };
                        m_log.addMember(PMember->id);
                        hitch(PMember);
                        anyChange = true;
                    }
                    else
                    {
                        if (it->second.job != job || it->second.level != level)
                        {
                            it->second.job   = job;
                            it->second.level = level;
                            anyChange        = true;
                        }
                        it->second.seenAt = now;
                    }
                    // Re-asserted every tick: another scope may have claimed
                    // her meanwhile
                    state.routes[PMember->id] = this;
                }
                for (auto it = m_roster.begin(); it != m_roster.end();)
                {
                    if (it->second.seenAt != now && now - it->second.seenAt >= kGrace)
                    {
                        const auto id = it->first;
                        ++it;
                        drop(id);
                        anyChange = false; // drop() already noted the change
                        continue;
                    }
                    ++it;
                }
                if (anyChange)
                {
                    changed(now);
                }
            }

            // The role holders' pace at the spot (role_support.h): one cycle
            // per stretch of fighting, from the first record opening to the
            // last closing; what each holder spent over the cycle against
            // her MP's net change since the last. Said once each way it turns
            void pace(const Conveyor::Scope& scope)
            {
                const auto closed = m_log.closedCount();
                if (closed > m_closedSeen)
                {
                    const auto newly = std::min<std::size_t>(closed - m_closedSeen, m_log.recent().size());
                    m_closedSeen     = closed;
                    for (std::size_t i = 0; i < newly; ++i)
                    {
                        for (const auto& m : m_log.recent()[i].members)
                        {
                            m_cycleSpent[m.id] += m.mpSpent;
                        }
                    }
                }
                const bool fighting = !m_log.open().empty();
                if (fighting == m_cycleOpen)
                {
                    return;
                }
                m_cycleOpen = fighting;
                for (auto* PMember : scope.members)
                {
                    if (!scope.holders.contains(PMember->id))
                    {
                        continue;
                    }
                    auto* PChar = static_cast<CCharEntity*>(PMember);
                    auto& pace  = m_pace[PChar->id];
                    if (fighting)
                    {
                        role::cycleOpened(PChar, pace);
                    }
                    else
                    {
                        role::cycleClosed(PChar, m_cycleSpent[PChar->id], pace);
                        role::speakPace(PChar, pace);
                    }
                }
                if (!fighting)
                {
                    m_cycleSpent.clear();
                }
            }

            uint32                                  m_scopeId;
            timer::time_point                       m_lastTick{};
            std::unordered_map<uint32, RosterEntry> m_roster;
            uint32                                  m_weakFrom = 0;
            FightLog                                m_log;
            Conveyor                                m_conveyor;
            std::unordered_map<uint32, cardian::tactics::Pace> m_pace;       // by role holder
            std::unordered_map<uint32, int32>                  m_cycleSpent; // by member, over the cycle under way
            bool                                    m_cycleOpen  = false;
            uint32                                  m_closedSeen = 0;
        };

        // Her scope: the alliance, else the party, else herself
        auto keyFor(const CCharEntity* PChar) -> std::pair<const void*, uint32>
        {
            if (PChar->PParty != nullptr)
            {
                if (PChar->PParty->m_PAlliance != nullptr)
                {
                    return { PChar->PParty->m_PAlliance, PChar->PParty->m_PAlliance->m_AllianceID };
                }
                return { PChar->PParty, PChar->PParty->GetPartyID() };
            }
            return { PChar, PChar->id };
        }

        auto find(const CCharEntity* PChar) -> Tactician*
        {
            const auto [key, id] = keyFor(PChar);
            const auto it        = state.tacticians.find(key);
            return it != state.tacticians.end() && it->second->scopeId() == id ? it->second.get() : nullptr;
        }

        // A world camp with no real player in it is watched only when asked
        auto watched(CCharEntity* PPawn) -> bool
        {
            static const bool world = settings::get<bool>("pawn.TACTICS_WORLD");
            return world || pawn::partyPlayer(PPawn) != nullptr || pawn::summonerOf(PPawn->id) != 0;
        }

        // --- the dispatcher: route by id, or drop ---------------------------

        // Every event is counted as kept or dropped: the dormant hitches'
        // traffic, made visible
        auto counted(Tactician* PTactician) -> Tactician*
        {
            ++(PTactician != nullptr ? state.routed : state.dropped);
            return PTactician;
        }

        auto routedMember(const CBattleEntity* PEntity) -> Tactician*
        {
            if (PEntity == nullptr || PEntity->objtype != TYPE_PC)
            {
                return nullptr;
            }
            const auto it = state.routes.find(PEntity->id);
            return it != state.routes.end() ? it->second : nullptr;
        }

        // A mob's own events go to every tactician with a record open on it
        // (two scopes on one mob each keep their own fight)
        template <typename F>
        void forEachRoutedMob(const CMobEntity* PMob, F&& f)
        {
            bool any = false;
            if (PMob != nullptr)
            {
                for (const auto& [key, PTactician] : state.tacticians)
                {
                    if (PTactician->log().hasOpen(PMob->id))
                    {
                        any = true;
                        f(*PTactician);
                    }
                }
            }
            ++(any ? state.routed : state.dropped);
        }

        void damaged(CBattleEntity* PTarget, const int32 amount, CBattleEntity* PAttacker, const xi::AttackType attackType)
        {
            if (auto* PMob = asMob(PTarget); PMob != nullptr)
            {
                // By the attacker when one of ours; else whoever has the fight
                // open counts what the mob lost (a damage-over-time tick)
                if (auto* PTactician = routedMember(PAttacker); PTactician != nullptr)
                {
                    counted(PTactician)->log().onMobDamaged(PMob, amount, PAttacker, attackType);
                }
                else
                {
                    forEachRoutedMob(PMob, [&](Tactician& tactician)
                                     {
                                         tactician.log().onMobDamaged(PMob, amount, PAttacker, attackType);
                                     });
                }
            }
            else if (auto* PTactician = counted(routedMember(PTarget)); PTactician != nullptr)
            {
                PTactician->log().onMemberDamaged(PTarget, amount, PAttacker);
            }
        }

        void magicStart(CBattleEntity* PCaster, CBattleEntity* PTarget, CLuaSpell* PLuaSpell)
        {
            if (auto* PTactician = counted(routedMember(PCaster)); PTactician != nullptr)
            {
                CSpell* PSpell = PLuaSpell != nullptr ? PLuaSpell->GetSpell() : nullptr;
                PTactician->log().onMagicStart(PCaster, PTarget, PSpell);
                if (PCaster->objtype == TYPE_PC)
                {
                    PTactician->conveyor().castStarted(static_cast<CCharEntity*>(PCaster), PSpell, PTarget != nullptr ? PTarget->id : 0);
                }
            }
        }

        void magicUse(CBattleEntity* PCaster, CBattleEntity* PTarget, CLuaSpell* PSpell, CLuaAction* PAction)
        {
            if (auto* PTactician = counted(routedMember(PCaster)); PTactician != nullptr)
            {
                PTactician->log().onMagicUse(PCaster, PTarget, PSpell, PAction);
                if (PCaster->objtype == TYPE_PC)
                {
                    CSpell*    PCast  = PSpell != nullptr ? PSpell->GetSpell() : nullptr;
                    const bool landed = PCast == nullptr || !PCast->isDebuff() || PCast->tookEffect();
                    PTactician->conveyor().castEnded(static_cast<CCharEntity*>(PCaster), PCast, PTarget != nullptr ? PTarget->id : 0, landed);
                }
            }
        }

        void magicInterrupted(CBattleEntity* PCaster)
        {
            if (auto* PTactician = counted(routedMember(PCaster)); PTactician != nullptr)
            {
                PTactician->log().onMagicInterrupted(PCaster);
                if (PCaster->objtype == TYPE_PC)
                {
                    PTactician->conveyor().castEnded(static_cast<CCharEntity*>(PCaster), nullptr, 0, false);
                }
            }
        }

        void attacked(CBattleEntity* PTarget, CBattleEntity* PAttacker)
        {
            if (auto* PTactician = counted(routedMember(PTarget)); PTactician != nullptr)
            {
                PTactician->log().onAttacked(PTarget, PAttacker);
            }
        }

        void engage(CBattleEntity* PMember, CBattleEntity* PTarget)
        {
            if (auto* PTactician = counted(routedMember(PMember)); PTactician != nullptr)
            {
                PTactician->log().onEngage(PMember, PTarget);
            }
        }

        void death(CBattleEntity* PEntity, CBattleEntity* PKiller)
        {
            if (auto* PMob = asMob(PEntity); PMob != nullptr)
            {
                forEachRoutedMob(PMob, [&](Tactician& tactician)
                                 {
                                     tactician.log().onMobDeath(PMob);
                                 });
            }
            else if (auto* PTactician = counted(routedMember(PEntity)); PTactician != nullptr)
            {
                PTactician->log().onMemberDeath(PEntity, PKiller);
            }
        }

        void tpMove(CBattleEntity* PEntity, const uint16 skillId)
        {
            auto* PMob = asMob(PEntity);
            forEachRoutedMob(PMob, [&](Tactician& tactician)
                             {
                                 tactician.log().onMobTpMove(PMob, skillId);
                             });
        }

        void paralyzed(CBattleEntity* PEntity)
        {
            if (auto* PMob = asMob(PEntity); PMob != nullptr)
            {
                forEachRoutedMob(PMob, [&](Tactician& tactician)
                                 {
                                     tactician.log().onMobParalyzed(PMob);
                                 });
            }
            else if (auto* PTactician = counted(routedMember(PEntity)); PTactician != nullptr)
            {
                PTactician->log().onMemberParalyzed(PEntity);
            }
        }

        // --- sol plumbing for the hitch ---------------------------------------

        // A C++ function as the Lua function the event handler stores. The
        // listeners capture nothing, so one function per event serves every
        // entity
        template <typename F>
        auto asFunction(F&& f) -> sol::function
        {
            return sol::make_object(::lua, sol::as_function(std::forward<F>(f))).template as<sol::function>();
        }

        auto entityOf(CLuaBaseEntity* PLua) -> CBattleEntity*
        {
            return PLua != nullptr ? dynamic_cast<CBattleEntity*>(PLua->GetBaseEntity()) : nullptr;
        }

        // An argument the server may pass as nil (no attacker on a
        // damage-over-time tick, no killer, no target): sol refuses nil
        // for a plain object parameter and throws the whole call away,
        // so every such slot is an optional
        auto entityOf(const sol::optional<CLuaBaseEntity*>& maybe) -> CBattleEntity*
        {
            return maybe.has_value() ? entityOf(*maybe) : nullptr;
        }
    } // namespace

    auto debug() -> bool
    {
        static const bool on = settings::get<bool>("pawn.TACTICS_DEBUG");
        return on;
    }

    void hitch(CBattleEntity* PEntity)
    {
        if (PEntity == nullptr || PEntity->PAI == nullptr || (PEntity->objtype != TYPE_PC && PEntity->objtype != TYPE_MOB))
        {
            return;
        }
        // Each lambda takes only the leading arguments it reads; sol ignores
        // the rest. Built once, shared by every hitched entity.
        static const sol::function onDamage = asFunction([](CLuaBaseEntity* PTarget, const int32 amount, const sol::optional<CLuaBaseEntity*> attacker, const sol::optional<uint16> attackType)
                                                         {
                                                             damaged(entityOf(PTarget), amount, entityOf(attacker), static_cast<xi::AttackType>(attackType.value_or(0)));
                                                         });
        static const sol::function onDeath = asFunction([](CLuaBaseEntity* PDead, const sol::optional<CLuaBaseEntity*> killer)
                                                        {
                                                            death(entityOf(PDead), entityOf(killer));
                                                        });
        static const sol::function onTpMove = asFunction([](CLuaBaseEntity* PMob, const sol::optional<uint16> skillId)
                                                         {
                                                             tpMove(entityOf(PMob), skillId.value_or(0));
                                                         });
        // A paralysis proc: the marked line in battleutils::IsParalyzed fires
        // it on whoever was stopped, a mob or one of ours (RESEARCH §12.13)
        static const sol::function onParalyzed = asFunction([](CLuaBaseEntity* PEntity)
                                                            {
                                                                paralyzed(entityOf(PEntity));
                                                            });
        // MAGIC_START fires from the magic state's init, before that state
        // is current: the spell comes from the event's own argument
        static const sol::function onMagicStart = asFunction([](CLuaBaseEntity* PCaster, const sol::optional<CLuaBaseEntity*> target, CLuaSpell* PSpell)
                                                             {
                                                                 magicStart(entityOf(PCaster), entityOf(target), PSpell);
                                                             });
        static const sol::function onMagicUse = asFunction([](CLuaBaseEntity* PCaster, const sol::optional<CLuaBaseEntity*> target, CLuaSpell* PSpell, CLuaAction* PAction)
                                                           {
                                                               magicUse(entityOf(PCaster), entityOf(target), PSpell, PAction);
                                                           });
        static const sol::function onMagicInterrupted = asFunction([](CLuaBaseEntity* PCaster)
                                                                   {
                                                                       magicInterrupted(entityOf(PCaster));
                                                                   });
        static const sol::function onAttacked = asFunction([](CLuaBaseEntity* PTarget, const sol::optional<CLuaBaseEntity*> attacker)
                                                           {
                                                               attacked(entityOf(PTarget), entityOf(attacker));
                                                           });
        static const sol::function onEngage = asFunction([](CLuaBaseEntity* PMember, const sol::optional<CLuaBaseEntity*> target)
                                                         {
                                                             engage(entityOf(PMember), entityOf(target));
                                                         });

        auto&      handler = PEntity->PAI->EventHandler;
        const bool mob     = PEntity->objtype == TYPE_MOB;
        (mob ? state.hitchedMobs : state.hitchedMembers).insert(PEntity->id);

        handler.addListener("TAKE_DAMAGE", onDamage, "cardian_tactics:TAKE_DAMAGE");
        handler.addListener("DEATH", onDeath, "cardian_tactics:DEATH");
        handler.addListener("PARALYZED", onParalyzed, "cardian_tactics:PARALYZED");
        if (mob)
        {
            handler.addListener("WEAPONSKILL_STATE_ENTER", onTpMove, "cardian_tactics:WEAPONSKILL_STATE_ENTER");
        }
        else
        {
            handler.addListener("MAGIC_START", onMagicStart, "cardian_tactics:MAGIC_START");
            handler.addListener("MAGIC_USE", onMagicUse, "cardian_tactics:MAGIC_USE");
            handler.addListener("MAGIC_INTERRUPTED", onMagicInterrupted, "cardian_tactics:MAGIC_INTERRUPTED");
            handler.addListener("ATTACKED", onAttacked, "cardian_tactics:ATTACKED");
            handler.addListener("ENGAGE", onEngage, "cardian_tactics:ENGAGE");
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} is hitched", PEntity->getName());
        }
    }

    void zoneIn(CCharEntity* PChar)
    {
        if (PChar != nullptr && state.routes.contains(PChar->id))
        {
            hitch(PChar);
        }
    }

    void memberLeft(const CBattleEntity* PMember, const CParty* PParty)
    {
        if (PMember == nullptr || PParty == nullptr)
        {
            return;
        }
        const void* key = PParty->m_PAlliance != nullptr ? static_cast<const void*>(PParty->m_PAlliance) : static_cast<const void*>(PParty);
        if (const auto it = state.tacticians.find(key); it != state.tacticians.end())
        {
            it->second->drop(PMember->id);
        }
    }

    void tick(CCharEntity* PPawn, const timer::time_point now)
    {
        if (PPawn == nullptr || !watched(PPawn))
        {
            return;
        }
        const auto [key, id] = keyFor(PPawn);
        auto it              = state.tacticians.find(key);
        // The same address, another party: never the old picture
        if (it != state.tacticians.end() && it->second->scopeId() != id)
        {
            state.tacticians.erase(it);
            it = state.tacticians.end();
        }
        if (it == state.tacticians.end())
        {
            it = state.tacticians.emplace(key, std::make_unique<Tactician>(id)).first;
        }
        it->second->tick(now, PPawn);

        // A tactician nobody has asked for in a minute is gone, and its
        // routes with it; what it still had open closes as "scope ended"
        if (now - state.lastSweep > 30s)
        {
            state.lastSweep = now;
            std::erase_if(state.tacticians, [&](const auto& kv)
                          {
                              return now - kv.second->lastTick() > 60s;
                          });
        }
    }

    auto entity(CCharEntity* PPawn, const uint32 id) -> CBattleEntity*
    {
        return PPawn != nullptr ? Conveyor::resolve(scopeOf(PPawn), id) : nullptr;
    }

    auto supportMage(CBattleEntity* PMember) -> bool
    {
        if (PMember == nullptr || PMember->objtype != TYPE_PC || PMember->PAI == nullptr)
        {
            return false;
        }
        auto* PController = dynamic_cast<CPawnController*>(PMember->PAI->GetController());
        return PController != nullptr && PController->Behavior(pawn::Behavior::Role).value_or(0) == static_cast<uint16>(pawn::Role::SupportMage);
    }

    auto has(const CCharEntity* PPawn) -> bool
    {
        return PPawn != nullptr && find(PPawn) != nullptr;
    }

    auto feed(CCharEntity* PPawn, CSpell* PSpell, CBattleEntity* PTarget, const uint32 row, const std::string& rowId) -> std::optional<Fed>
    {
        auto* PTactician = PPawn != nullptr && PTarget != nullptr ? find(PPawn) : nullptr;
        if (PTactician == nullptr)
        {
            return std::nullopt;
        }
        // Met already, or blocked by what is on the target: not fed. Said
        // once per caster, row and target under debug, since the row asks
        // every think
        const auto on      = bank::onAlready(PSpell, PTarget);
        const bool blocked = !on.has_value() && bank::blockedOn(PSpell, PTarget);
        if (on.has_value() || blocked)
        {
            if (debug())
            {
                static std::unordered_set<std::string> said;
                if (said.size() > 4096)
                {
                    said.clear();
                }
                if (said.insert(fmt::format("{}:{}:{}", PPawn->id, rowId, PTarget->id)).second)
                {
                    ShowInfoFmt("tactics: {}'s row {}: {} is {} {}{}, not fed", PPawn->getName(), row, PSpell->getName(),
                                blocked ? "blocked by what is on" : "on", PTarget->getName(),
                                blocked ? std::string("") : (std::isinf(*on) ? std::string(" already, for good") : fmt::format(" already, {:.0f} s to go", *on)));
                }
            }
            return std::nullopt;
        }
        const auto& n = PTactician->conveyor().feed(Conveyor::keyFor(PSpell, PTarget->id, PPawn->id),
                                                    Request{ .source = Source::Row,
                                                             .caster = PPawn->id,
                                                             .row    = row,
                                                             .rowId  = rowId,
                                                             .spell  = PSpell != nullptr ? static_cast<uint16>(PSpell->getID()) : uint16(0),
                                                             .fedAt  = seconds(timer::now()) },
                                                    scopeOf(PPawn));
        Fed fed;
        fed.mine = n.assigned == PPawn->id && n.lockedBy == 0;
        if (fed.mine)
        {
            fed.spell  = static_cast<SpellID>(n.spell);
            fed.target = n.key.target;
            fed.why    = fmt::format("her row {}", row);
        }
        return fed;
    }

    auto assignment(CCharEntity* PPawn, const bool engaged) -> std::optional<Assignment>
    {
        auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr;
        if (PTactician == nullptr)
        {
            return std::nullopt;
        }
        const auto a = PTactician->conveyor().assignment(PPawn->id, engaged, scopeOf(PPawn));
        if (!a.has_value())
        {
            return std::nullopt;
        }
        return Assignment{ a->spell, a->target, a->why };
    }

    void roleReflex(CCharEntity* PPawn)
    {
        if (auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr; PTactician != nullptr)
        {
            role::reflex(PPawn, PTactician->log(), PTactician->conveyor(), scopeOf(PPawn), seconds(timer::now()));
        }
    }

    void roleThink(CCharEntity* PPawn, const bool engaged)
    {
        if (auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr; PTactician != nullptr)
        {
            role::think(PPawn, PTactician->log(), PTactician->conveyor(), scopeOf(PPawn), engaged, seconds(timer::now()));
        }
    }

    auto lines(CCharEntity* PChar) -> std::vector<std::string>
    {
        std::vector<std::string> out;
        if (PChar == nullptr)
        {
            return out;
        }
        const uint16 zone = PChar->loc.zone != nullptr ? static_cast<uint16>(PChar->loc.zone->GetID()) : 0;
        const auto   now  = seconds(timer::now());

        // The stake (RESEARCH §12.16), and whether the plan holds the party to it
        if (const auto stake = pawn::stakeOf(PChar->id); stake.has_value())
        {
            auto* PZone = zoneutils::GetZone(stake->zone);
            out.push_back(fmt::format("stake: {} at ({:.0f}, {:.0f}), facing {} deg; the plan is {}", PZone != nullptr ? PZone->getName() : "?",
                                      stake->at.x, stake->at.z, stake->at.rotation * 360 / 256, pawn::strategyOf(PChar->id) == 1 ? "on (the party keeps to it)" : "off (they follow you)"));
        }

        auto* PTactician = find(PChar);
        if (PTactician == nullptr)
        {
            out.push_back("tactics: no cardian of yours has ticked here yet");
        }
        else
        {
            const auto& log = PTactician->log();
            out.push_back(fmt::format("tactics: {} watched, the guess is {}, {} fight{} on record", PTactician->roster().size(), PTactician->guessWeak() ? "weak" : "strong",
                                      log.closedCount(), log.closedCount() == 1 ? "" : "s"));
            for (const auto& r : log.open())
            {
                out.push_back(fmt::format("open: {} for {:.0f} s, took {}, dealt {}, cures {} MP{}", r.mobName, r.seconds(now), r.taken(), r.dealt(), r.cureMp(), r.overlapping ? ", linked" : ""));
            }
            for (const auto& line : log.priceLists())
            {
                out.push_back(line);
            }
            const auto  scope = scopeOf(PChar);
            std::string holders;
            for (auto* PMember : scope.members)
            {
                if (scope.holders.contains(PMember->id))
                {
                    holders += (holders.empty() ? "" : ", ") + PMember->getName();
                }
            }
            out.push_back(fmt::format("conveyor: {} need{}, Support Mage held by {}", PTactician->conveyor().needs().size(), PTactician->conveyor().needs().size() == 1 ? "" : "s", holders.empty() ? "nobody" : holders));
            for (const auto& line : PTactician->conveyor().lines(scope))
            {
                out.push_back(line);
            }
            std::size_t shown = 0;
            for (const auto& r : log.recent())
            {
                if (shown++ == 3)
                {
                    break;
                }
                out.push_back(cardian::tactics::summary(r));
            }
        }

        for (const auto& line : spotLines(zone))
        {
            out.push_back("here: " + line);
        }
        if (PTactician != nullptr)
        {
            for (const auto& [id, entry] : PTactician->roster())
            {
                for (const auto& line : cureLines(id, entry.name))
                {
                    out.push_back("cures: " + line);
                }
            }
        }
        else
        {
            for (const auto& line : cureLines(PChar->id, PChar->getName()))
            {
                out.push_back("cures: " + line);
            }
        }
        for (const auto& line : debuffLines(zone))
        {
            out.push_back("debuffs: " + line);
        }
        out.push_back(fmt::format("plumbing: {} members and {} mobs hitched since boot, {} routes, {} tactician{}, {} events routed, {} dropped",
                                  state.hitchedMembers.size(), state.hitchedMobs.size(), state.routes.size(), state.tacticians.size(), state.tacticians.size() == 1 ? "" : "s", state.routed, state.dropped));

        // The chat cuts a long line; a summary is wrapped at a clause, the
        // continuation indented. The map log gets the same lines unwrapped,
        // so the command's answer is readable without the client
        std::vector<std::string> wrapped;
        for (const auto& line : out)
        {
            std::string rest = line;
            while (rest.size() > kChatWidth)
            {
                auto cut = rest.rfind(", ", kChatWidth);
                if (const auto semi = rest.rfind("; ", kChatWidth); semi != std::string::npos && (cut == std::string::npos || semi > cut))
                {
                    cut = semi;
                }
                if (cut == std::string::npos || cut < kChatWidth / 2)
                {
                    cut = kChatWidth;
                }
                wrapped.push_back(rest.substr(0, cut + 1));
                rest = "  " + rest.substr(cut + 1);
                while (rest.size() > 2 && rest[2] == ' ')
                {
                    rest.erase(2, 1);
                }
            }
            wrapped.push_back(rest);
        }
        for (const auto& line : out)
        {
            ShowInfoFmt("tactics: !tactics: {}", line);
        }
        return wrapped;
    }
} // namespace pawn::tactics
