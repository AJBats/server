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

#include "bank_math.h"
#include "conveyor.h"
#include "cure_math.h"
#include "fight_log.h"
#include "cure_readiness.h"
#include "party_roster.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_gambits.h"
#include "role_support.h"
#include "rest_math.h"
#include "rest_policy.h"
#include "spell_bank.h"
#include "tank_calls.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"

#include "ability.h"
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
#include "recast_container.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
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
        constexpr auto        kLive      = 2s;  // a tactician not ticked this long is no conveyor: nothing would clear its locks

        // pawn.TACTICS_REQUEST_LIFE: a request not re-fed this long is withdrawn
        auto requestLife() -> double
        {
            static const double life = settings::get<float>("pawn.TACTICS_REQUEST_LIFE");
            return life;
        }

        // pawn.TACTICS_FIRST_AID_FLOOR, as a fraction of max HP
        auto firstAidFloor() -> double
        {
            static const double floor = settings::get<float>("pawn.TACTICS_FIRST_AID_FLOOR") / 100.0;
            return floor;
        }

        // Her scope as the game holds it this instant: the alliance's
        // characters, and which of them offer the party spells. Built fresh
        // for every call; nothing keeps an entity pointer across ticks
        auto scopeOf(CCharEntity* PChar) -> Conveyor::Scope
        {
            Conveyor::Scope scope;
            PChar->ForAlliance([&](CBattleEntity* PMember)
                               {
                                   if (PMember != nullptr && PMember->objtype == TYPE_PC)
                                   {
                                       scope.members.push_back(PMember);
                                       if (offersSpells(PMember))
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

            auto resting() -> RestPlanner& { return m_rest; }

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
                auto emergency = chooseFirstAid(scope, seconds(now));
                m_cureStarted  = false;
                m_rest.tick(m_log, scope, seconds(now), emergency);
                m_conveyor.emergency(std::move(emergency));
                m_conveyor.tick(seconds(now), requestLife(), scope);
                pace(scope);
            }

            // A cure started since first aid was last chosen (MAGIC_START,
            // before its magic state is current): the choice is made again
            // before anyone reads it, with that cure in flight, so no mage
            // casts first aid it has already met
            void cureStarted()
            {
                m_cureStarted = true;
            }
            void freshFirstAid(CCharEntity* PAsker)
            {
                if (!m_cureStarted)
                {
                    return;
                }
                m_cureStarted = false;
                m_conveyor.emergency(chooseFirstAid(scopeOf(PAsker), seconds(timer::now())));
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

            auto fightUnderWay() -> bool
            {
                return std::any_of(m_log.open().begin(), m_log.open().end(), [](const auto& r) { return !r.settling(); });
            }

            // The members as first aid weighs them while a fight is under
            // way: each one's danger (role::threat), never less than the
            // floor (withFloor). Nobody while no fight is
            auto riskTargets(const Conveyor::Scope& scope, const double at) -> std::vector<cardian::cure::Target>
            {
                std::vector<cardian::cure::Target> targets;
                if (!fightUnderWay())
                {
                    return targets;
                }
                for (auto* member : scope.members)
                {
                    if (member == nullptr || member->isDead()) continue;
                    const auto threat = role::threat(m_log, member, at);
                    targets.push_back(cardian::cure::withFloor({member->id, static_cast<double>(member->health.hp), static_cast<double>(member->GetMaxHP()),
                        threat.biggestHit, threat.takenPerSecond}, firstAidFloor()));
                }
                return targets;
            }

            // First aid (cure_math.h choose): the cures each mage could
            // land and those in flight, against each member's danger -- the
            // last choice keeping an emergency through its approach
            auto chooseFirstAid(const Conveyor::Scope& scope, const double at) -> std::vector<cardian::cure::Choice>
            {
                return cardian::cure::choose(measureCures(scope, at), riskTargets(scope, at), m_conveyor.emergencies());
            }

            // A cardian mage on her feet, whose next cure a kneel beside her
            // can count on: not kneeling herself, and not a played character,
            // whose casting nothing promises
            static auto onHerFeet(const Conveyor::Scope& scope, const uint32 id) -> bool
            {
                auto* PBody = dynamic_cast<CCharEntity*>(Conveyor::resolve(scope, id));
                auto* gambits = PBody != nullptr ? pawn::gambitsOf(PBody) : nullptr;
                return gambits != nullptr && !gambits->Host().OwnClient() && !PBody->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
            }

            // How long the fight has left at the party's rate (bank_math.h
            // remainingLife), the longest-lived of its mobs, and that mob;
            // negative when no mob's life is known
            auto fightLeft(const double at) -> std::pair<double, uint32>
            {
                std::pair<double, uint32> out{ -1.0, 0 };
                for (const auto& r : m_log.open())
                {
                    auto* PMob = r.settling() ? nullptr : dynamic_cast<CMobEntity*>(zoneutils::GetEntity(r.mobId, TYPE_MOB));
                    if (PMob == nullptr || PMob->isDead())
                    {
                        continue;
                    }
                    const double left = cardian::tactics::remainingLife(PMob->health.hp, r.dealtPerSecond(at), r.seconds(at), spotAverages(r.zone, r.mobName).dealtPerSecond.mean);
                    if (out.second == 0 || left > out.first)
                    {
                        out = { left, r.mobId };
                    }
                }
                return out;
            }

        public:
            // The kneel's question (tactics.h kneelRisk): the cures promised
            // to land are those in flight and each other cardian mage's
            // earliest on each member; the kneeler's own are what the kneel
            // gives up
            auto kneelRisk(const Conveyor::Scope& scope, const CCharEntity* PKneeler, const double at) -> std::optional<KneelRisk>
            {
                const auto targets = riskTargets(scope, at);
                if (targets.empty())
                {
                    return std::nullopt;
                }
                const auto measured = measureCures(scope, at);
                std::vector<cardian::cure::Option>                          promised;
                std::unordered_map<uint64, const cardian::cure::Option*> earliest; // by (caster, target)
                for (const auto& option : measured)
                {
                    if (option.inFlight)
                    {
                        promised.push_back(option);
                        continue;
                    }
                    if (option.caster == PKneeler->id || !std::isfinite(option.time.land) || option.heals <= 0.0 || !onHerFeet(scope, option.caster))
                    {
                        continue;
                    }
                    auto& best = earliest[(static_cast<uint64>(option.caster) << 32) | option.target];
                    if (best == nullptr || option.time.land < best->time.land)
                    {
                        best = &option;
                    }
                }
                for (const auto& [key, option] : earliest)
                {
                    promised.push_back(*option);
                }
                const double horizon = cardian::rest::kKneelSeconds + 2.0 * settings::get<uint8>("map.HEALING_TICK_DELAY");
                const auto   risk    = cardian::cure::atRisk(targets, horizon, promised);
                if (!risk.has_value())
                {
                    return std::nullopt;
                }
                const auto* PMember        = Conveyor::resolve(scope, risk->id);
                const auto [left, mob]     = fightLeft(at);
                return KneelRisk{ .member = risk->id, .name = PMember != nullptr ? PMember->getName() : "?", .hp = risk->hp,
                                  .perSecond = risk->damageRate, .biggest = risk->biggest, .horizon = horizon, .fightLeft = left, .mob = mob };
            }

        private:

            void changed(const timer::time_point)
            {
                m_weakFrom = m_log.closedCount();
                m_rest.reset(m_log.closedCount());
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
            RestPlanner                             m_rest;
            std::unordered_map<uint32, cardian::tactics::Pace> m_pace;       // by role holder
            std::unordered_map<uint32, int32>                  m_cycleSpent; // by member, over the cycle under way
            bool                                    m_cycleOpen  = false;
            uint32                                  m_closedSeen = 0;
            bool                                    m_cureStarted = false; // first aid to choose again before it is read
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

        // A world camp with no real player in it is watched only when asked;
        // a played character's party, his alone included, always is
        auto watched(CCharEntity* PPawn) -> bool
        {
            static const bool world = settings::get<bool>("pawn.TACTICS_WORLD");
            return world || PPawn->PSession != nullptr || pawn::partyPlayer(PPawn) != nullptr || pawn::summonerOf(PPawn->id) != 0;
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
                PTactician->resting().observe(PCaster, seconds(timer::now()));
                CSpell* PSpell = PLuaSpell != nullptr ? PLuaSpell->GetSpell() : nullptr;
                PTactician->log().onMagicStart(PCaster, PTarget, PSpell);
                if (PCaster->objtype == TYPE_PC)
                {
                    auto* PChar = static_cast<CCharEntity*>(PCaster);
                    PTactician->conveyor().castStarted(PChar, PSpell, PTarget != nullptr ? PTarget->id : 0, scopeOf(PChar));
                    if (PSpell != nullptr && PSpell->getSpellFamily() == SPELLFAMILY_CURE)
                    {
                        PTactician->cureStarted();
                    }
                }
            }
        }

        void magicUse(CBattleEntity* PCaster, CBattleEntity* PTarget, CLuaSpell* PSpell, CLuaAction* PAction)
        {
            if (auto* PTactician = counted(routedMember(PCaster)); PTactician != nullptr)
            {
                // Sample at spell completion as well as each party tick so a
                // later recovery tick does not conceal the spell's MP loss.
                PTactician->resting().observe(PCaster, seconds(timer::now()));
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
        // entity. The handle is never destroyed: its destructor releases a
        // Lua reference, and exit handlers run after the Lua state is gone
        template <typename F>
        auto asFunction(F&& f) -> const sol::function&
        {
            return *new sol::function(sol::make_object(::lua, sol::as_function(std::forward<F>(f))).template as<sol::function>());
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
        static const sol::function& onDamage = asFunction([](CLuaBaseEntity* PTarget, const int32 amount, const sol::optional<CLuaBaseEntity*> attacker, const sol::optional<uint16> attackType)
                                                          {
                                                              damaged(entityOf(PTarget), amount, entityOf(attacker), static_cast<xi::AttackType>(attackType.value_or(0)));
                                                          });
        static const sol::function& onDeath = asFunction([](CLuaBaseEntity* PDead, const sol::optional<CLuaBaseEntity*> killer)
                                                         {
                                                             death(entityOf(PDead), entityOf(killer));
                                                         });
        static const sol::function& onTpMove = asFunction([](CLuaBaseEntity* PMob, const sol::optional<uint16> skillId)
                                                          {
                                                              tpMove(entityOf(PMob), skillId.value_or(0));
                                                          });
        // A paralysis proc: the marked line in battleutils::IsParalyzed fires
        // it on whoever was stopped, a mob or one of ours (RESEARCH §12.13)
        static const sol::function& onParalyzed = asFunction([](CLuaBaseEntity* PEntity)
                                                             {
                                                                 paralyzed(entityOf(PEntity));
                                                             });
        // MAGIC_START fires from the magic state's init, before that state
        // is current: the spell comes from the event's own argument
        static const sol::function& onMagicStart = asFunction([](CLuaBaseEntity* PCaster, const sol::optional<CLuaBaseEntity*> target, CLuaSpell* PSpell)
                                                              {
                                                                  magicStart(entityOf(PCaster), entityOf(target), PSpell);
                                                              });
        static const sol::function& onMagicUse = asFunction([](CLuaBaseEntity* PCaster, const sol::optional<CLuaBaseEntity*> target, CLuaSpell* PSpell, CLuaAction* PAction)
                                                            {
                                                                magicUse(entityOf(PCaster), entityOf(target), PSpell, PAction);
                                                            });
        static const sol::function& onMagicInterrupted = asFunction([](CLuaBaseEntity* PCaster)
                                                                    {
                                                                        magicInterrupted(entityOf(PCaster));
                                                                    });
        static const sol::function& onAttacked = asFunction([](CLuaBaseEntity* PTarget, const sol::optional<CLuaBaseEntity*> attacker)
                                                            {
                                                                attacked(entityOf(PTarget), entityOf(attacker));
                                                            });
        static const sol::function& onEngage = asFunction([](CLuaBaseEntity* PMember, const sol::optional<CLuaBaseEntity*> target)
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

    // Each member's answer is her gambit engine's, whoever drives her
    // (pawn::gambitsOf): a cardian's and a played character's alike
    auto offersSpells(CBattleEntity* PMember) -> bool
    {
        auto* PGambits = pawn::gambitsOf(PMember);
        return PGambits != nullptr && PGambits->MasterOn() && PGambits->OffersSpells();
    }

    auto offersRest(CBattleEntity* PMember) -> bool
    {
        auto* PGambits = pawn::gambitsOf(PMember);
        return PGambits != nullptr && PGambits->MasterOn() && PGambits->OffersRest();
    }

    auto nukesOf(CBattleEntity* PMember) -> std::vector<SpellID>
    {
        auto* PGambits = pawn::gambitsOf(PMember);
        return PGambits != nullptr && PGambits->MasterOn() ? PGambits->OfferedNukes() : std::vector<SpellID>{};
    }

    auto attendsFight(CBattleEntity* PMember, CBattleEntity* PMob) -> bool
    {
        auto* PGambits = pawn::gambitsOf(PMember);
        return PGambits != nullptr && PGambits->AttendsFight(PMob);
    }

    auto admittedBy(CBattleEntity* PHolder, const SpellID spell, CBattleEntity* PTarget) -> std::optional<std::string>
    {
        auto* PGambits = pawn::gambitsOf(PHolder);
        return PGambits != nullptr ? PGambits->Admits(static_cast<uint16>(spell), PTarget) : std::nullopt;
    }

    auto allows(CBattleEntity* PHolder, const SpellID spell) -> bool
    {
        auto* PGambits = pawn::gambitsOf(PHolder);
        return PGambits != nullptr && PGambits->AllowsSpell(static_cast<uint16>(spell));
    }

    auto has(const CCharEntity* PPawn) -> bool
    {
        const auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr;
        return PTactician != nullptr && timer::now() - PTactician->lastTick() <= kLive;
    }

    auto feed(CCharEntity* PPawn, CSpell* PSpell, CBattleEntity* PTarget, const uint32 row, const std::string& rowId) -> std::optional<Fed>
    {
        auto* PTactician = PPawn != nullptr && PTarget != nullptr ? find(PPawn) : nullptr;
        if (PTactician == nullptr)
        {
            return std::nullopt;
        }
        // Keep effect coordination separate from Cure's HP policy: a
        // debuff already met (or nullified by an active effect) is not fed
        // again. This check does not reject a Cure on a full-HP target.
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
        PTactician->freshFirstAid(PPawn);
        const auto& n = PTactician->conveyor().feed(Conveyor::keyFor(PSpell, PTarget->id, PPawn->id),
                                                    Request{ .source = Source::Row,
                                                             .caster = PPawn->id,
                                                             .row    = row,
                                                             .rowId  = rowId,
                                                             .spell  = PSpell != nullptr ? static_cast<uint16>(PSpell->getID()) : uint16(0),
                                                             .fedAt  = seconds(timer::now()) },
                                                    scopeOf(PPawn));
        Fed fed;
        fed.mine = n.assigned == PPawn->id; // a locked need is assigned only as a cure's top-up
        if (fed.mine)
        {
            fed.spell  = static_cast<SpellID>(n.spell);
            fed.target = n.key.target;
            fed.why    = fmt::format("her row {}", row);
        }
        return fed;
    }

    auto othersCasting(CCharEntity* PPawn, CSpell* PSpell, CBattleEntity* PTarget) -> bool
    {
        auto* PTactician = PPawn != nullptr && PSpell != nullptr && PTarget != nullptr ? find(PPawn) : nullptr;
        if (PTactician == nullptr)
        {
            return false;
        }
        const auto key = Conveyor::keyFor(PSpell, PTarget->id, PPawn->id);
        return std::ranges::any_of(PTactician->conveyor().needs(), [&key, PPawn](const Need& n)
                                   {
                                       return n.key == key && n.lockedBy != 0 && n.lockedBy != PPawn->id;
                                   });
    }

    auto assignment(CCharEntity* PPawn, const bool engaged) -> std::optional<Assignment>
    {
        auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr;
        if (PTactician == nullptr)
        {
            return std::nullopt;
        }
        PTactician->freshFirstAid(PPawn);
        const auto a = PTactician->conveyor().assignment(PPawn->id, engaged, scopeOf(PPawn));
        if (!a.has_value())
        {
            return std::nullopt;
        }
        return Assignment{ a->spell, a->target, a->why, a->approach, a->emergency };
    }

    void roleThink(CCharEntity* PPawn, const bool engaged)
    {
        if (auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr; PTactician != nullptr)
        {
            PTactician->freshFirstAid(PPawn);
            role::think(PPawn, PTactician->log(), PTactician->conveyor(), scopeOf(PPawn), engaged, seconds(timer::now()));
        }
    }

    auto nukePrices(CCharEntity* PPawn, CBattleEntity* PTarget, const std::vector<SpellID>& spells) -> std::optional<cardian::tactics::NukePricing>
    {
        auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr;
        auto* PMob       = dynamic_cast<CMobEntity*>(PTarget);
        if (PTactician == nullptr || PMob == nullptr)
        {
            return std::nullopt;
        }
        for (auto& r : PTactician->log().open())
        {
            if (r.mobId != PMob->id || r.settling())
            {
                continue;
            }
            std::vector<CSpell*> asked;
            for (const auto id : spells)
            {
                if (auto* PSpell = spell::GetSpell(id); PSpell != nullptr)
                {
                    asked.push_back(PSpell);
                }
            }
            const auto  scope  = scopeOf(PPawn);
            const auto& spot   = spotAverages(r.zone, r.mobName);
            auto        priced = bank::priceNukes(r, spot, scope.members, PPawn, PMob, asked);
            return priced.has_value() ? std::move(*priced) : cardian::tactics::NukePricing{};
        }
        return std::nullopt;
    }

    void noteSneakAttack(CCharEntity* PPawn, const uint32 mobId, const cardian::tactics::SneakUse use, const double seconds)
    {
        if (auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr; PTactician != nullptr)
        {
            PTactician->log().onSneakAttack(PPawn, mobId, use, seconds);
        }
    }

    auto tankCall(CCharEntity* PPawn, const bool engaged) -> std::optional<TankCall>
    {
        auto* PTactician = PPawn != nullptr ? find(PPawn) : nullptr;
        auto* PProvoke   = ability::GetAbility(ABILITY_PROVOKE);
        if (PTactician == nullptr || PProvoke == nullptr)
        {
            return std::nullopt;
        }
        static const int holdHp = settings::get<int>("pawn.TANK_PROVOKE_HOLD_HP");

        cardian::tank::View v;
        v.self = PPawn->id;
        // HasRecast reads the clock; Has only says an entry exists, and one
        // outlives its recast
        const bool hers = charutils::hasAbility(PPawn, ABILITY_PROVOKE);
        v.provokeReady  = hers && !PPawn->PRecastContainer->HasRecast(RECAST_ABILITY, PProvoke->getRecastId(), PProvoke->getRecastTime());
        v.provokeRange  = PProvoke->getRange();
        v.holdHp        = holdHp;

        // The party as it stands: who is the Healer by the screen's role,
        // and how hurt each is
        const auto scope = scopeOf(PPawn);
        for (auto* PMember : scope.members)
        {
            if (PMember != nullptr && !PMember->isDead())
            {
                auto* PChar = static_cast<CCharEntity*>(PMember);
                v.members.push_back({ PChar->id, PChar->GetHPP(), pawn::roster::roleOf(PChar) == cardian::party::Role::Healer });
            }
        }

        // The mobs the party fights, as the log has them open, each read
        // live: its HP, whom it is on, how far from her -- hitbox to hitbox,
        // which is the gap the server's own range check allows. Her own
        // fight is among them whether or not the log has it yet; a fight of
        // the alliance's in another zone is nothing to her
        auto* PFight = engaged ? PPawn->GetBattleTarget() : nullptr;
        if (PFight != nullptr && !PFight->isDead())
        {
            v.fight = PFight->id;
        }
        const auto add = [&](CBattleEntity* PMob)
        {
            if (PMob == nullptr || PMob->isDead() || PMob->loc.zone != PPawn->loc.zone ||
                std::ranges::any_of(v.mobs, [&](const cardian::tank::Mob& m) { return m.id == PMob->id; }))
            {
                return;
            }
            auto*        POn    = PMob->GetBattleTarget();
            const uint32 target = POn != nullptr && POn->objtype == TYPE_PC ? POn->id : 0;
            const float  gap    = distance(PPawn->loc.p, PMob->loc.p) - PPawn->modelHitboxSize - PMob->modelHitboxSize;
            v.mobs.push_back({ PMob->id, PMob->GetHPP(), target, gap });
        };
        for (const auto& r : PTactician->log().open())
        {
            if (!r.settling())
            {
                add(asMob(Conveyor::resolve(scope, r.mobId)));
            }
        }
        add(PFight);

        // Its mind in words, for her engine to log as it changes (as the
        // bank says its prices): the call, or why it holds Provoke. Provoke
        // merely on its clock is every fight's rhythm, said by the use line
        // already: quiet. Provoke not hers at all is said once
        const auto decision = cardian::tank::decide(v);
        TankCall   out;
        if (decision.call.has_value())
        {
            auto* PMob = Conveyor::resolve(scope, decision.call->mob);
            out.mob    = decision.call->mob;
            out.why    = std::string(cardian::tank::reasonName(decision.call->reason));
            out.mind   = fmt::format("Provoke {} ({})", PMob != nullptr ? PMob->getName() : "?", out.why);
        }
        else if (decision.held == cardian::tank::Held::NoProvoke)
        {
            out.quiet = hers;
            out.mind  = hers ? "holds Provoke (on its clock)" : "holds Provoke (not hers at all)";
        }
        else
        {
            out.mind = fmt::format("holds Provoke ({}; {} fight{} in the party's picture)",
                                   cardian::tank::heldName(decision.held), v.mobs.size(), v.mobs.size() == 1 ? "" : "s");
        }
        return out;
    }

    auto restAdvice(CCharEntity* PPawn) -> std::optional<RestAdvice>
    {
        auto* tactician = PPawn != nullptr ? find(PPawn) : nullptr;
        if (tactician != nullptr)
        {
            // Another member may have advanced the shared planner before
            // this body's Healing tick. Price the MP it actually has now.
            tactician->resting().observe(PPawn, seconds(timer::now()));
        }
        return tactician != nullptr ? tactician->resting().advice(PPawn->id) : std::nullopt;
    }

    auto kneelRisk(CCharEntity* PPawn) -> std::optional<KneelRisk>
    {
        auto* tactician = PPawn != nullptr ? find(PPawn) : nullptr;
        return tactician != nullptr ? tactician->kneelRisk(scopeOf(PPawn), PPawn, seconds(timer::now())) : std::nullopt;
    }

    auto recoveryDue(const CCharEntity* PPawn) -> bool
    {
        auto* tactician = PPawn != nullptr ? find(PPawn) : nullptr;
        const auto advice = tactician != nullptr ? tactician->resting().advice(PPawn->id) : std::nullopt;
        return advice.has_value() && advice->recover;
    }

    void resetRestMemory(CCharEntity* PPawn)
    {
        if (auto* tactician = PPawn != nullptr ? find(PPawn) : nullptr; tactician != nullptr)
        {
            tactician->resting().reset(tactician->log().closedCount());
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

        // The camp holds independently of Hold/Pull; Retreat suspends it.
        if (const auto stake = pawn::stakeOf(PChar->id); stake.has_value())
        {
            auto* PZone = zoneutils::GetZone(stake->zone);
            out.push_back(fmt::format("camp: {} at ({:.0f}, {:.0f}), facing {} deg; {}; orders {}", PZone != nullptr ? PZone->getName() : "?",
                                      stake->at.x, stake->at.z, stake->at.rotation * 360 / 256,
                                      pawn::isRetreating(PChar->id) ? "retreat active" : "the party keeps to it", pawn::strategyName(pawn::strategyOf(PChar->id))));
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
            for (const auto& line : PTactician->resting().lines(scope))
            {
                out.push_back(line);
            }
            std::string holders;
            for (auto* PMember : scope.members)
            {
                if (scope.holders.contains(PMember->id))
                {
                    holders += (holders.empty() ? "" : ", ") + PMember->getName();
                }
            }
            out.push_back(fmt::format("conveyor: {} need{}, spells offered by {}", PTactician->conveyor().needs().size(), PTactician->conveyor().needs().size() == 1 ? "" : "s", holders.empty() ? "nobody" : holders));
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
