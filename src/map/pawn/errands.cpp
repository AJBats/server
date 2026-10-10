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

#include "errands.h"

#include "cardian_link.h"
#include "club.h"
#include "club_math.h"
#include "gate_guards.h"
#include "link_api.h"
#include "pawn.h"
#include "pawn_travel.h"
#include "professions.h"
#include "redress.h"
#include "seats.h"
#include "supplies.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/mmo.h"
#include "common/timer.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "lua/lua_base_entity.h"
#include "navmesh/navmesh.h"
#include "packets/s2c/0x017_chat_std.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace pawn::errands
{
    namespace
    {
        using namespace cardian::link;
        using cardian::errand::Errand;
        using cardian::errand::GearStep;
        using cardian::errand::Kind;
        using cardian::errand::Member;
        using cardian::errand::State;

        constexpr auto  kTickEvery   = std::chrono::seconds(1);
        constexpr auto  kLeaveWithin = std::chrono::seconds(120); // the walk to her zone line: past it she leaves the world where she stands
        constexpr auto  kStepWalk    = std::chrono::seconds(120); // a walk of the gear-up's: past it the next step
        constexpr auto  kCensusWait  = std::chrono::seconds(60);  // the census's answer at the counter: past it she goes on without
        constexpr float kAtNpc       = 5.0f;                      // yalms from the counter or the guard: she stands at it
        constexpr float kAtLine      = 4.0f;                      // from her zone line's centre: she is at it
        constexpr float kAtSpot      = 2.0f;                      // from where she started: she is back
        constexpr float kLineSnap    = 12.0f;                     // a zone line's centre sits off the mesh by this much at most
        constexpr float kShortOf     = 2.5f;                      // a counter or a guard is walked to this far short, on her side

        // The live half of an errand she walks: the map's alone, gone with a
        // restart (a walk the restart ended is ended)
        struct Walk
        {
            GearStep                   step = GearStep::ToCounter; // gear up's
            timer::time_point          since{};                    // the step's start, on the simulation clock (a pause holds it)
            std::optional<position_t>  goal;                       // where she walks now
            position_t                 there{};                    // what she walks to: the counter, the guard, her line, her spot
            float                      within = kAtSpot;           // how close to `there` counts as there
            bool                       asked = false;              // the census was asked at the counter
            const pawn::guards::Guard* guard = nullptr;            // the guard she buys from
            bool                       refused   = false;          // another nation's guard turned her away
            bool                       consulate = false;          // on to her nation's consulate in another zone of her city
            uint16                     guardZone = 0;              // the consulate's zone
        };

        struct Record
        {
            uint32                 charid       = 0;
            uint32                 accid        = 0;
            uint32                 playerCharID = 0;
            Errand                 errand;
            cardian::errand::Args  args;
            std::string            title;
            uint16                 fromZone = 0; // where she started: a gear-up comes back here
            position_t             from{};
            Walk                   walk;
        };

        std::unordered_map<uint32, Record> records;
        // The errands that ended as her player logged in as her, by kind, to
        // tell him once she is in (endForLogin, zonedIn)
        std::unordered_map<uint32, Kind> endedAtLogin;
        timer::time_point                  tickedAt{};

        auto gameNow() -> uint32
        {
            return earth_time::game_timestamp();
        }

        auto nameOf(const uint32 charid) -> std::string
        {
            if (const auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn->getName();
            }
            return pawn::seats::nameOf(charid);
        }

        // The player who sent her, told a line in his chat log when he is here
        void tell(const uint32 playerCharID, const std::string& text)
        {
            if (auto* PPlayer = zoneutils::GetChar(playerCharID); PPlayer != nullptr && !pawn::isPawn(PPlayer))
            {
                PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, text);
            }
        }

        // ---- the row -----------------------------------------------------------

        void save(const Record& r)
        {
            const auto& e = r.errand;
            db::preparedStmt("INSERT INTO cardian_errands (charid, accid, player_charid, kind, args, title, state, started, left_at, ends, target, progress, "
                             "from_zone, from_x, from_y, from_z, from_rot) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                             "ON DUPLICATE KEY UPDATE accid = VALUES(accid), player_charid = VALUES(player_charid), kind = VALUES(kind), args = VALUES(args), "
                             "title = VALUES(title), state = VALUES(state), started = VALUES(started), left_at = VALUES(left_at), ends = VALUES(ends), "
                             "target = VALUES(target), progress = VALUES(progress), from_zone = VALUES(from_zone), from_x = VALUES(from_x), "
                             "from_y = VALUES(from_y), from_z = VALUES(from_z), from_rot = VALUES(from_rot)",
                             r.charid, r.accid, r.playerCharID, std::string(cardian::errand::kindName(e.kind)), cardian::errand::argsText(r.args), r.title,
                             std::string(cardian::errand::stateName(e.state)), e.started, e.left, e.ends, e.target, e.progress, r.fromZone, r.from.x, r.from.y,
                             r.from.z, r.from.rotation);
        }

        void erase(const uint32 charid)
        {
            db::preparedStmt("DELETE FROM cardian_errands WHERE charid = ?", charid);
            records.erase(charid);
        }

        // ---- the quests and missions -------------------------------------------

        // An entry of the errand table (modules/cardian/lua/errand_quests.lua)
        struct Goal
        {
            bool                  mission = false;
            uint8                 log     = 0;
            uint16                id      = 0;
            std::string           title;
            uint16                minutes = 0;
            std::optional<uint8>  job; // the job it unlocks (0: the support jobs)
            std::vector<uint16_t> route;
        };

        // The entries he has done; nothing when the table could not be asked
        auto goalsOf(CCharEntity* PPlayer) -> std::optional<std::vector<Goal>>
        {
            auto call  = pawn::linkapi::libraryCall("errands", "goals");
            auto found = call ? pawn::linkapi::libraryTable("errands.goals", (*call)(CLuaBaseEntity(PPlayer))) : std::nullopt;
            if (!found)
            {
                return std::nullopt;
            }
            std::vector<Goal> out;
            for (std::size_t i = 1; i <= found->size(); ++i)
            {
                const sol::object entry = (*found)[i];
                if (entry.get_type() != sol::type::table)
                {
                    continue;
                }
                const auto row = entry.as<sol::table>();
                Goal       goal;
                goal.mission = row.get_or<std::string>("kind", "") == "mission";
                goal.log     = row.get_or<uint8>("log", 0);
                goal.id      = row.get_or<uint16>("id", 0);
                goal.title   = row.get_or<std::string>("title", "");
                goal.minutes = row.get_or<uint16>("minutes", 60);
                if (const sol::object job = row["job"]; job.get_type() == sol::type::number)
                {
                    goal.job = job.as<uint8>();
                }
                if (const sol::object zones = row["zones"]; zones.get_type() == sol::type::table)
                {
                    const auto list = zones.as<sol::table>();
                    for (std::size_t z = 1; z <= list.size(); ++z)
                    {
                        if (const sol::object zone = list[z]; zone.get_type() == sol::type::number)
                        {
                            goal.route.push_back(zone.as<uint16_t>());
                        }
                    }
                }
                out.push_back(std::move(goal));
            }
            return out;
        }

        // Her log as it stands: her body's when she has one, else her row's
        struct Log
        {
            questlog_t   quests[MAX_QUESTAREA]{};
            missionlog_t missions[MAX_MISSIONAREA]{};
            uint8        nation = 0;
        };

        auto loadedChar(const uint32 charid) -> CCharEntity*
        {
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn;
            }
            return zoneutils::GetChar(charid);
        }

        auto logOf(const uint32 charid) -> std::optional<Log>
        {
            Log log;
            if (const auto* PChar = loadedChar(charid); PChar != nullptr)
            {
                std::copy(std::begin(PChar->m_questLog), std::end(PChar->m_questLog), std::begin(log.quests));
                std::copy(std::begin(PChar->m_missionLog), std::end(PChar->m_missionLog), std::begin(log.missions));
                log.nation = PChar->profile.nation;
                return log;
            }
            const auto rset = db::preparedStmt("SELECT quests, missions, nation FROM chars WHERE charid = ?", charid);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            db::extractFromBlob(rset, "quests", log.quests);
            db::extractFromBlob(rset, "missions", log.missions);
            log.nation = rset->get<uint8>("nation");
            return log;
        }

        // She has done it already
        auto doneAlready(const Log& log, const Goal& goal) -> bool
        {
            if (goal.mission)
            {
                return goal.log < MAX_MISSIONAREA && cardian::errand::missionDone(log.missions[goal.log].complete, goal.id);
            }
            return goal.log < MAX_QUESTAREA && cardian::errand::questDone(log.quests[goal.log].complete, goal.id);
        }

        // An entry she can be sent on: in a log she has, one she has not
        // done, and a nation's mission only for one of that nation
        auto eligible(const Log& log, const Goal& goal) -> bool
        {
            if (goal.mission ? goal.log >= MAX_MISSIONAREA || (goal.log <= 2 && goal.log != log.nation) : goal.log >= MAX_QUESTAREA)
            {
                return false;
            }
            return !doneAlready(log, goal);
        }

        // The job columns of char_jobs, by job id
        constexpr std::array<const char*, 23> kJobColumns{ "", "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
                                                            "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run" };
        constexpr std::array<const char*, 23> kJobNames{ "Support jobs", "Warrior", "Monk", "White Mage", "Black Mage", "Red Mage", "Thief", "Paladin",
                                                          "Dark Knight", "Beastmaster", "Bard", "Ranger", "Samurai", "Ninja", "Dragoon", "Summoner",
                                                          "Blue Mage", "Corsair", "Puppetmaster", "Dancer", "Scholar", "Geomancer", "Rune Fencer" };

        // The quest or the mission done in her own log, and the job it
        // unlocks unlocked (as the game's unlockJob: the support jobs keep
        // Warrior's level at 1 at least). On her body when she has one --
        // she should not, being away -- else on her rows
        void complete(const Record& r)
        {
            const bool mission = cardian::errand::numberArg(r.args, "goal") == static_cast<uint32_t>(CL_GOAL_MISSION);
            const auto log     = static_cast<uint8>(cardian::errand::numberArg(r.args, "log").value_or(0));
            const auto id      = static_cast<uint16>(cardian::errand::numberArg(r.args, "id").value_or(0));
            const auto job     = cardian::errand::numberArg(r.args, "job");
            if ((mission && log >= MAX_MISSIONAREA) || (!mission && log >= MAX_QUESTAREA))
            {
                return;
            }
            const uint16 noMission = log > 2 ? 0 : 65535; // a nation's log rests on 65535, the later ones on 0
            const bool   unlocks   = job.has_value() && *job < kJobColumns.size();
            const uint8  jobId     = unlocks ? static_cast<uint8>(*job) : 0;
            const uint8  levelled  = jobId == 0 ? static_cast<uint8>(xi::Job::WAR) : jobId; // whose level unlockJob raises to 1

            if (auto* PChar = loadedChar(r.charid); PChar != nullptr)
            {
                if (mission)
                {
                    auto& entry = PChar->m_missionLog[log];
                    if (cardian::errand::markMissionDone(entry.current, entry.complete, id, noMission))
                    {
                        charutils::SaveMissionsList(PChar);
                    }
                }
                else if (cardian::errand::markQuestDone(PChar->m_questLog[log].current, PChar->m_questLog[log].complete, id))
                {
                    charutils::SaveQuestsList(PChar);
                }
                if (unlocks)
                {
                    PChar->jobs.unlocked |= (1 << jobId);
                    if (PChar->jobs.job[levelled] == 0)
                    {
                        PChar->jobs.job[levelled] = 1;
                    }
                    charutils::SaveCharJob(PChar, static_cast<xi::Job>(levelled));
                }
                return;
            }

            const auto rset = db::preparedStmt("SELECT quests, missions FROM chars WHERE charid = ?", r.charid);
            if (!rset || !rset->next())
            {
                ShowErrorFmt("errands: {}'s log could not be read; her {} is not written", nameOf(r.charid), r.title);
                return;
            }
            if (mission)
            {
                missionlog_t missions[MAX_MISSIONAREA]{};
                db::extractFromBlob(rset, "missions", missions);
                if (cardian::errand::markMissionDone(missions[log].current, missions[log].complete, id, noMission))
                {
                    db::preparedStmt("UPDATE chars SET missions = ? WHERE charid = ? LIMIT 1", missions, r.charid);
                }
            }
            else
            {
                questlog_t quests[MAX_QUESTAREA]{};
                db::extractFromBlob(rset, "quests", quests);
                if (cardian::errand::markQuestDone(quests[log].current, quests[log].complete, id))
                {
                    db::preparedStmt("UPDATE chars SET quests = ? WHERE charid = ? LIMIT 1", quests, r.charid);
                }
            }
            if (unlocks)
            {
                db::preparedStmt(fmt::format("UPDATE char_jobs SET unlocked = unlocked | ?, {0} = GREATEST({0}, 1) WHERE charid = ? LIMIT 1", kJobColumns[levelled]),
                                 static_cast<uint32>(1u << jobId), r.charid);
            }
        }

        // ---- the rank catch-up -----------------------------------------------------

        constexpr std::array<const char*, 3> kRankColumns{ "rank_sandoria", "rank_bastok", "rank_windurst" };
        constexpr uint16                     kNoNationMission = 65535; // a nation's log at rest

        // A nation's missions on the errand table, the ladder of its ranks
        // (xi.cardian.errands.ladder), with their titles; read once, the
        // table being the module's own
        struct Ladder
        {
            std::vector<cardian::club::Mission> missions;
            std::vector<std::string>            titles;
        };

        auto ladderOf(const uint8 nation) -> const Ladder&
        {
            static std::array<std::optional<Ladder>, 3> read;
            static const Ladder                         none;
            if (nation >= read.size())
            {
                return none;
            }
            if (read[nation].has_value())
            {
                return *read[nation];
            }
            auto call  = pawn::linkapi::libraryCall("errands", "ladder");
            auto found = call ? pawn::linkapi::libraryTable("errands.ladder", (*call)(nation)) : std::nullopt;
            if (!found)
            {
                return none; // asked again next time
            }
            Ladder ladder;
            for (std::size_t i = 1; i <= found->size(); ++i)
            {
                const sol::object entry = (*found)[i];
                if (entry.get_type() != sol::type::table)
                {
                    continue;
                }
                const auto              row = entry.as<sol::table>();
                cardian::club::Mission  mission;
                mission.id      = row.get_or<uint16>("id", 0);
                mission.step    = row.get_or<uint8>("step", 0);
                mission.minutes = row.get_or<uint16>("minutes", 30);
                if (const sol::object zones = row["zones"]; zones.get_type() == sol::type::table)
                {
                    const auto list = zones.as<sol::table>();
                    for (std::size_t z = 1; z <= list.size(); ++z)
                    {
                        if (const sol::object zone = list[z]; zone.get_type() == sol::type::number)
                        {
                            mission.zones.push_back(zone.as<uint16_t>());
                        }
                    }
                }
                ladder.missions.push_back(std::move(mission));
                ladder.titles.push_back(row.get_or<std::string>("title", ""));
            }
            read[nation] = std::move(ladder);
            return *read[nation];
        }

        // The catch-up of her log to a rank: nothing when the rank is not
        // between hers and the ladder's top, or nothing of it is left to do
        auto rankPlanFor(const Log& log, const uint8 rankNow, const uint8 target) -> std::optional<cardian::club::RankPlan>
        {
            if (log.nation >= kRankColumns.size())
            {
                return std::nullopt;
            }
            const auto& entry = log.missions[log.nation];
            return cardian::club::rankPlan(ladderOf(log.nation).missions, rankNow, target, [&](const uint16 id)
                                           { return cardian::errand::missionDone(entry.complete, id); });
        }

        // The first `count` missions of her catch-up done in her nation's log,
        // as the game's own completions leave it: each done, and her current
        // mission, one of those or none, none (between missions, as a
        // nation's completion leaves her); her rank the highest those
        // missions grant and the target's once all are done (raised only),
        // her rank bar empty at a new rank. On her body when she has one,
        // else on her rows. The rank reached, 0 when nothing was done
        auto applyRank(const Record& r, std::size_t count) -> uint8
        {
            const auto nation   = static_cast<uint8>(cardian::errand::numberArg(r.args, "nation").value_or(0));
            const auto target   = static_cast<uint8>(cardian::errand::numberArg(r.args, "rank").value_or(0));
            const auto missions = cardian::errand::numbersOf(r.args.contains("missions") ? r.args.at("missions") : std::string{});
            const auto grants   = cardian::errand::numbersOf(r.args.contains("grants") ? r.args.at("grants") : std::string{});
            count               = std::min(count, missions.size());
            if (count == 0 || nation >= kRankColumns.size())
            {
                return 0;
            }
            uint8 reached = 0;
            for (std::size_t i = 0; i < count && i < grants.size(); ++i)
            {
                reached = std::max<uint8>(reached, static_cast<uint8>(grants[i]));
            }
            if (count == missions.size())
            {
                reached = std::max(reached, target);
            }
            const uint16 last = missions[count - 1];

            const auto mark = [&](missionlog_t& entry)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (missions[i] < cardian::errand::kMissionsPerLog)
                    {
                        entry.complete[missions[i]] = true;
                    }
                }
                if (entry.current != kNoNationMission && entry.current <= last)
                {
                    entry.current     = kNoNationMission;
                    entry.statusUpper = 0;
                    entry.statusLower = 0;
                }
            };

            if (auto* PChar = loadedChar(r.charid); PChar != nullptr)
            {
                mark(PChar->m_missionLog[nation]);
                if (reached > PChar->profile.rank[nation])
                {
                    PChar->profile.rank[nation] = reached;
                    PChar->profile.rankpoints   = 0;
                }
                charutils::SaveMissionsList(PChar);
                return reached;
            }
            const auto rset = db::preparedStmt("SELECT missions FROM chars WHERE charid = ?", r.charid);
            if (!rset || !rset->next())
            {
                ShowErrorFmt("errands: {}'s mission log could not be read; her catch-up is not written", nameOf(r.charid));
                return 0;
            }
            missionlog_t log[MAX_MISSIONAREA]{};
            db::extractFromBlob(rset, "missions", log);
            mark(log[nation]);
            db::preparedStmt("UPDATE chars SET missions = ? WHERE charid = ? LIMIT 1", log, r.charid);
            if (reached > 0)
            {
                db::preparedStmt(fmt::format("UPDATE char_profile SET rank_points = 0 WHERE charid = ? AND {} < ? LIMIT 1", kRankColumns[nation]), r.charid, reached);
                db::preparedStmt(fmt::format("UPDATE char_profile SET {0} = ? WHERE charid = ? AND {0} < ? LIMIT 1", kRankColumns[nation]), reached, r.charid, reached);
            }
            return reached;
        }

        // ---- where she walks ---------------------------------------------------

        auto snap(CZone* PZone, const position_t& point, const float within) -> std::optional<position_t>
        {
            const auto* navMesh = PZone != nullptr ? PZone->navMesh() : nullptr;
            if (navMesh == nullptr)
            {
                return point;
            }
            const auto snapped = navMesh->findClosestValidPoint(point);
            if (!snapped.has_value() || distance(*snapped, point) > within)
            {
                return std::nullopt;
            }
            return *snapped;
        }

        // A step short of an NPC, on her side of it, on the zone's floor: a
        // counter or a guard often stands behind a desk the mesh leaves out
        auto shortOf(CZone* PZone, const position_t& npc, const position_t& from) -> std::optional<position_t>
        {
            position_t  want = npc;
            const float dx   = from.x - npc.x;
            const float dz   = from.z - npc.z;
            const float len  = std::sqrt(dx * dx + dz * dz);
            if (len > 0.01f)
            {
                want.x += dx / len * kShortOf;
                want.z += dz / len * kShortOf;
            }
            if (auto at = snap(PZone, want, kAtNpc); at.has_value())
            {
                return at;
            }
            return snap(PZone, npc, kAtNpc + kShortOf);
        }

        // The zone line she leaves by: toward the first zone of her route
        // that is not hers, where a walk leads there
        auto lineToward(const CCharEntity* PPawn, const std::vector<uint16_t>& route) -> std::optional<position_t>
        {
            const auto here = PPawn->getZone();
            for (const auto zone : route)
            {
                if (zone == static_cast<uint16>(here))
                {
                    continue;
                }
                if (const auto hop = pawn::travel::nextHop(here, static_cast<xi::ZoneId>(zone), PPawn->loc.p); hop.has_value())
                {
                    return snap(PPawn->loc.zone, hop->walkTo, kLineSnap);
                }
            }
            return std::nullopt;
        }

        // The nearest auction counter of her zone
        auto counterIn(CCharEntity* PPawn) -> const CBaseEntity*
        {
            const CBaseEntity* PNearest = nullptr;
            float              nearest  = 0.0f;
            for (const auto* PCounter : PPawn->loc.zone->queryEntitiesByName("Auction_Counter"))
            {
                if (PCounter == nullptr)
                {
                    continue;
                }
                const float away = distance(PPawn->loc.p, PCounter->loc.p);
                if (PNearest == nullptr || away < nearest)
                {
                    PNearest = PCounter;
                    nearest  = away;
                }
            }
            return PNearest;
        }

        void walkTo(Record& r, const position_t& there, const std::optional<position_t>& goal, const float within)
        {
            r.walk.there = there;
            r.walk.goal  = goal;
            r.walk.within = within;
            r.walk.since = timer::now();
            if (goal.has_value())
            {
                pawn::setWalkOrder(r.charid, *goal, pawn::kErrandWalker);
            }
            else
            {
                pawn::clearWalkOrder(r.charid);
            }
        }

        // ---- the rabbit hole -----------------------------------------------------

        // Gone from the world: off the clock no more, out of any party, and
        // the ladder takes her body, keeping her online
        void leaveWorld(Record& r, const char* how)
        {
            pawn::clearWalkOrder(r.charid);
            if (auto* PPawn = pawn::findPawn(r.charid); PPawn != nullptr && PPawn->PParty != nullptr)
            {
                PPawn->PParty->RemoveMember(PPawn);
            }
            const auto minutes = cardian::errand::numberArg(r.args, "minutes").value_or(60);
            r.errand           = cardian::errand::goAway(r.errand, gameNow(), minutes * 60);
            save(r);
            pawn::seats::run();
            ShowInfoFmt("errands: {} leaves the world {} for {} ({} minutes on the game clock){}", nameOf(r.charid), how, r.title, minutes,
                        pawn::findPawn(r.charid) != nullptr ? "; the ladder has not taken her body" : "");
        }

        // Back: what the errand earned put on her -- written before she can
        // stand, so the body the ladder loads has it -- the row gone, and the
        // ladder stands her where she left the world when her player is near.
        // `say`: her player told how it ended
        void finish(const uint32 charid, const cardian::errand::Kept kept, const bool calledBack, const bool say = true)
        {
            const auto it = records.find(charid);
            if (it == records.end())
            {
                return;
            }
            const Record r    = it->second;
            const auto   name = nameOf(charid);
            if (r.errand.state == State::Going)
            {
                pawn::clearWalkOrder(charid);
                pawn::clearTravelOrder(charid); // a gear-up on its way to her consulate, or back
            }

            std::string line;
            if (r.errand.kind == Kind::Quest && kept == cardian::errand::Kept::Whole)
            {
                complete(r);
                line = fmt::format("{} is back: {} is complete.", name, r.title);
                if (const auto job = cardian::errand::numberArg(r.args, "job"); job.has_value() && *job < kJobNames.size())
                {
                    line += *job == 0 ? " Support jobs are unlocked." : fmt::format(" {} is unlocked.", kJobNames[*job]);
                }
            }
            else if (r.errand.kind == Kind::Rank && kept != cardian::errand::Kept::Nothing)
            {
                // all of it once due, else what her time away has covered
                const auto missions = cardian::errand::numbersOf(r.args.contains("missions") ? r.args.at("missions") : std::string{});
                const auto minutes  = cardian::errand::numbersOf(r.args.contains("mins") ? r.args.at("mins") : std::string{});
                const auto away     = gameNow() > r.errand.left ? static_cast<uint64_t>(gameNow() - r.errand.left) : 0;
                const auto count    = kept == cardian::errand::Kept::Whole ? missions.size() : cardian::errand::missionsDone(minutes, away);
                const auto reached  = applyRank(r, count);
                if (count == 0)
                {
                    line = fmt::format("{} is recalled before the first mission is done.", name);
                }
                else if (kept == cardian::errand::Kept::Whole)
                {
                    line = fmt::format("{} is back: rank {}.", name, reached);
                }
                else
                {
                    line = fmt::format("{} is recalled with {} mission{} done{}.", name, count, count == 1 ? "" : "s",
                                       reached > 0 ? fmt::format(", at rank {}", reached) : std::string{});
                }
            }
            else if (r.errand.kind == Kind::Gear && !calledBack)
            {
                line = fmt::format("{} is back from the auction house.", name);
            }
            else if (r.errand.kind == Kind::Money)
            {
                // where she left, saved again before the ladder stands her by it
                if (r.args.contains("placed") && r.args.at("placed") == "1" && r.fromZone != 0)
                {
                    db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_x = ?, pos_y = ?, pos_z = ?, pos_rot = ? WHERE charid = ?", r.fromZone, r.from.x, r.from.y,
                                     r.from.z, r.from.rotation, charid);
                }
                const bool recalled = r.args.contains("recalled") && r.args.at("recalled") == "1";
                line                = r.errand.state == State::Going ? fmt::format("{} is recalled.", name)
                                      : recalled                     ? fmt::format("{} is recalled from fishing.", name)
                                                                     : fmt::format("{} is back from fishing.", name);
            }
            else
            {
                line = fmt::format("{} is recalled.", name);
            }

            erase(charid);
            pawn::seats::touch(charid);
            pawn::seats::run();
            ShowInfoFmt("errands: {} ({}) is back from her {} errand{}", name, charid, cardian::errand::kindName(r.errand.kind),
                        kept == cardian::errand::Kept::Whole ? ", done" : (calledBack ? ", called back" : ""));
            if (say)
            {
                tell(r.playerCharID, line);
            }
        }

        // ---- a money venture --------------------------------------------------

        // Her saved place moved to the spot she works, so search finds her
        // there while she is faded. Where she left is the row's: read from
        // her saved place first when she left with no body
        void place(Record& r)
        {
            if (r.fromZone == 0)
            {
                if (const auto rset = db::preparedStmt("SELECT pos_zone, pos_x, pos_y, pos_z, pos_rot FROM chars WHERE charid = ?", r.charid); rset && rset->next())
                {
                    r.fromZone = rset->get<uint16>("pos_zone");
                    r.from     = position_t(rset->get<float>("pos_x"), rset->get<float>("pos_y"), rset->get<float>("pos_z"), 0, rset->get<uint8>("pos_rot"));
                }
            }
            const auto profession = static_cast<uint8>(cardian::errand::numberArg(r.args, "profession").value_or(0));
            const auto zone       = static_cast<uint16>(cardian::errand::numberArg(r.args, "zone").value_or(0));
            const auto area       = static_cast<uint16>(cardian::errand::numberArg(r.args, "area").value_or(0));
            const auto spot       = pawn::professions::spotOf(r.charid, profession, zone, area);
            const auto at         = spot.has_value() ? spot->centre : position_t{};
            save(r); // where she left, kept before her saved place moves
            db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_x = ?, pos_y = ?, pos_z = ? WHERE charid = ?", zone, at.x, at.y, at.z, r.charid);
            r.args["placed"] = "1";
            save(r);
            pawn::seats::touch(r.charid);
            ShowInfoFmt("errands: {} is faded at {} (zone {}, area {}) while she works", nameOf(r.charid), r.title, zone, area);
        }

        // The venture over, recalled or due: the keeper counts her takings and
        // marks the row back, and she stands only then. Only a row still away
        // is moved: the keeper may have ended the trip itself (a rod broken)
        void returning(Record& r, const bool recalled)
        {
            if (recalled)
            {
                r.args["recalled"] = "1";
            }
            const auto moved = db::preparedStmt("UPDATE cardian_errands SET state = 'returning', args = ? WHERE charid = ? AND state = 'away'",
                                                cardian::errand::argsText(r.args), r.charid);
            r.errand.state = moved && moved->rowsAffected() == 1 ? State::Returning : State::Back;
            ShowInfoFmt("errands: {}'s venture at {} is over{}; {}", nameOf(r.charid), r.title, recalled ? " (called back)" : "",
                        r.errand.state == State::Returning ? "the venture keeper counts her takings" : "the keeper ended it already");
        }

        // The keeper has counted her takings: its word is the row's state
        auto markedBack(const uint32 charid) -> bool
        {
            const auto rset = db::preparedStmt("SELECT state FROM cardian_errands WHERE charid = ?", charid);
            return rset && rset->next() && rset->get<std::string>("state") == "back";
        }

        void tickMoney(Record& r, const uint32 clock)
        {
            const auto charid = r.charid;
            if (r.errand.state == State::Away)
            {
                if ((!r.args.contains("placed") || r.args.at("placed") != "1") && pawn::findPawn(charid) == nullptr)
                {
                    place(r);
                }
                if (cardian::errand::due(r.errand, clock))
                {
                    returning(r, false);
                }
                else if (markedBack(charid)) // the keeper ended the trip itself: a rod broke
                {
                    r.errand.state = State::Back;
                }
                return;
            }
            if (r.errand.state == State::Back || markedBack(charid))
            {
                r.errand.state = State::Back;
                finish(charid, cardian::errand::Kept::Whole, false);
            }
        }

        // ---- gear up -----------------------------------------------------------

        // One gear-up step entered: her walk, the census asked, her scrolls bought
        void enterGear(Record& r, CCharEntity* PPawn, const GearStep step)
        {
            r.walk.step = step;
            switch (step)
            {
                case GearStep::AtCounter:
                    walkTo(r, PPawn->loc.p, std::nullopt, kAtNpc);
                    r.walk.asked = pawn::redress::askFor(PPawn);
                    ShowInfoFmt("errands: {} is at the auction counter{}", PPawn->getName(), r.walk.asked ? "; the census is asked to dress her" : ", dressed for her level already");
                    break;
                case GearStep::ToGuard:
                {
                    // No guard in her zone sells to her -- another nation's,
                    // outranking hers -- or one turned her away: straight to
                    // her nation's consulate in another zone of her city, by
                    // the zone line, and the walk to its guard once she is
                    // there (tickGear)
                    const auto local      = pawn::guards::guardFor(PPawn->loc.zone, PPawn->profile.nation, PPawn->loc.p);
                    const bool localSells = local.has_value() && local->guard != nullptr && pawn::guards::sellsTo(*local->guard, PPawn->profile.nation);
                    if (!r.walk.consulate && (r.walk.refused || !localSells))
                    {
                        if (const auto consulate = pawn::guards::consulateFor(PPawn->loc.zone, PPawn->profile.nation);
                            consulate.has_value() && pawn::orderTravel(r.charid, consulate->zone, 0))
                        {
                            r.walk.consulate = true;
                            r.walk.guard     = consulate->guard;
                            r.walk.guardZone = consulate->zone;
                            walkTo(r, PPawn->loc.p, std::nullopt, kAtNpc);
                            ShowInfoFmt("errands: {} goes to her nation's consulate, {} in zone {} ({})", PPawn->getName(), consulate->guard->name, consulate->zone,
                                        r.walk.refused ? "turned away here" : "no guard here sells to her");
                            break;
                        }
                    }
                    r.walk.guard = local.has_value() ? local->guard : nullptr;
                    if (local.has_value() && local->npc != nullptr)
                    {
                        walkTo(r, local->npc->loc.p, shortOf(PPawn->loc.zone, local->npc->loc.p, PPawn->loc.p), kAtNpc);
                    }
                    else
                    {
                        walkTo(r, PPawn->loc.p, std::nullopt, kAtNpc); // nowhere to walk: the step is over at once
                    }
                    break;
                }
                case GearStep::AtGuard:
                {
                    walkTo(r, PPawn->loc.p, std::nullopt, kAtNpc);
                    // Only another nation's guard's refusal sends her on to her
                    // consulate, whose stock is the same
                    std::string  refusal;
                    const uint32 bought = r.walk.guard != nullptr ? pawn::supplies::buyAt(PPawn, *r.walk.guard, zoneutils::GetChar(r.playerCharID), &refusal) : 0;
                    r.walk.refused      = bought == 0 && (refusal == "OUTRANKED" || refusal == "FOREIGN_PLACE");
                    ShowInfoFmt("errands: {} is at {}: {} scroll{} bought", PPawn->getName(), r.walk.guard != nullptr ? r.walk.guard->name : "no guard", bought,
                                bought == 1 ? "" : "s");
                    break;
                }
                case GearStep::Back:
                    if (static_cast<uint16>(PPawn->getZone()) != r.fromZone)
                    {
                        pawn::orderTravel(r.charid, r.fromZone, 0); // back from her consulate: the walk to her spot once there (tickGear)
                        walkTo(r, PPawn->loc.p, std::nullopt, kAtSpot);
                        break;
                    }
                    walkTo(r, r.from, snap(PPawn->loc.zone, r.from, kAtNpc), kAtSpot);
                    break;
                default:
                    break;
            }
        }

        void tickGear(Record& r)
        {
            auto* PPawn = pawn::findPawn(r.charid);
            // Her own zone, or on the way to her consulate and back: another
            // zone of her city, or the zone line between
            const bool trekking = pawn::travelOrderOf(r.charid).has_value();
            const auto zoneNow  = PPawn != nullptr && PPawn->loc.zone != nullptr ? static_cast<uint16>(PPawn->getZone()) : 0;
            const bool inPlace  = zoneNow == r.fromZone || (r.walk.consulate && (trekking || zoneNow == r.walk.guardZone));
            if (PPawn == nullptr || PPawn->loc.zone == nullptr || PPawn->isDead() || !inPlace)
            {
                const auto   name   = nameOf(r.charid);
                const uint32 player = r.playerCharID;
                ShowInfoFmt("errands: {} is gone from where she was gearing up; the errand ends", name);
                finish(r.charid, cardian::errand::Kept::Nothing, true, false);
                tell(player, fmt::format("{} could not finish gearing up.", name));
                return;
            }
            const auto* PPlayer = zoneutils::GetChar(r.playerCharID);
            const bool  withHim = PPlayer != nullptr && PPlayer->loc.zone == PPawn->loc.zone && PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty;
            const auto  step    = r.walk.step;
            const auto  timeFor = step == GearStep::AtCounter ? std::chrono::duration_cast<timer::duration>(kCensusWait)
                                  : r.walk.consulate      ? std::chrono::duration_cast<timer::duration>(kStepWalk * 4)
                                                          : std::chrono::duration_cast<timer::duration>(kStepWalk);

            // Off the zone line, in the zone she was going to: the walk there
            if (r.walk.consulate && !trekking && !r.walk.goal.has_value() && cardian::errand::walks(step))
            {
                if (step == GearStep::ToGuard && zoneNow == r.walk.guardZone && r.walk.guard != nullptr)
                {
                    const auto npcs = PPawn->loc.zone->queryEntitiesByName(std::string(r.walk.guard->name));
                    if (!npcs.empty() && npcs.front() != nullptr)
                    {
                        walkTo(r, npcs.front()->loc.p, shortOf(PPawn->loc.zone, npcs.front()->loc.p, PPawn->loc.p), kAtNpc);
                    }
                }
                else if (step == GearStep::Back && zoneNow == r.fromZone)
                {
                    walkTo(r, r.from, snap(PPawn->loc.zone, r.from, kAtNpc), kAtSpot);
                }
            }

            cardian::errand::GearFacts facts;
            const bool walkingThere = !trekking && (!r.walk.consulate || zoneNow == (step == GearStep::Back ? r.fromZone : r.walk.guardZone) || !cardian::errand::walks(step));
            facts.arrived     = walkingThere && distance(PPawn->loc.p, r.walk.there) <= r.walk.within;
            facts.timedOut    = timer::now() - r.walk.since >= timeFor || (cardian::errand::walks(step) && !trekking && !r.walk.goal.has_value());
            facts.dressed     = !r.walk.asked || pawn::redress::settled(r.charid);
            facts.hasGuard    = pawn::guards::guardFor(PPawn->loc.zone, PPawn->profile.nation, PPawn->loc.p).has_value() ||
                             pawn::guards::consulateFor(PPawn->loc.zone, PPawn->profile.nation).has_value();
            facts.returns     = !withHim;
            facts.toConsulate = r.walk.refused && !r.walk.consulate && pawn::guards::consulateFor(PPawn->loc.zone, PPawn->profile.nation).has_value();

            const auto next = cardian::errand::nextGearStep(step, facts);
            if (next == GearStep::Done)
            {
                finish(r.charid, cardian::errand::Kept::Whole, false);
                return;
            }
            if (next != step)
            {
                enterGear(r, PPawn, next);
                return;
            }
            // A walk the player took off her (his ring), given again
            if (cardian::errand::walks(step) && r.walk.goal.has_value() && !pawn::walkOrderOf(r.charid).has_value())
            {
                pawn::setWalkOrder(r.charid, *r.walk.goal, pawn::kErrandWalker);
            }
        }

        // ---- the Link ----------------------------------------------------------

        auto sendGear(CCharEntity* PPlayer, Record r) -> uint16
        {
            if (const auto why = gearRefusal(r.charid); why != CL_S_OK)
            {
                return why;
            }
            auto*       PPawn    = pawn::findPawn(r.charid);
            const auto* PCounter = counterIn(PPawn);
            if (PCounter == nullptr)
            {
                return CL_S_NO_AUCTION_HOUSE;
            }
            r.fromZone = static_cast<uint16>(PPawn->getZone());
            r.from     = PPawn->loc.p;
            records[r.charid] = r;
            auto& kept        = records[r.charid];
            walkTo(kept, PCounter->loc.p, shortOf(PPawn->loc.zone, PCounter->loc.p, PPawn->loc.p), kAtNpc);
            save(kept);
            ShowInfoFmt("errands: {} sends {} to gear up at the auction house in {}", PPlayer->getName(), PPawn->getName(), PPawn->loc.zone->getName());
            tell(PPlayer->id, fmt::format("{} heads for the auction house.", PPawn->getName()));
            return CL_S_OK;
        }

        // An errand that leaves the world set out: standing, she leaves his
        // party and walks to the zone line toward her route's first other
        // zone (where she stands when none leads there); with no body she is
        // away at once
        auto setOut(CCharEntity* PPlayer, Record r, const std::string& setsOut = {}) -> uint16
        {
            const auto title  = r.title;
            const auto charid = r.charid;
            records[charid]   = std::move(r);
            auto& kept        = records[charid];
            auto* PPawn       = pawn::findPawn(charid);
            if (PPawn != nullptr && PPawn->loc.zone != nullptr)
            {
                if (PPawn->PParty != nullptr)
                {
                    PPawn->PParty->RemoveMember(PPawn);
                }
                kept.fromZone = static_cast<uint16>(PPawn->getZone());
                kept.from     = PPawn->loc.p;
                if (const auto line = lineToward(PPawn, kept.errand.route); line.has_value())
                {
                    walkTo(kept, *line, line, kAtLine);
                    save(kept);
                    ShowInfoFmt("errands: {} sends {} on {}: she walks to the zone line at ({:.0f}, {:.0f}, {:.0f})", PPlayer->getName(), PPawn->getName(), title,
                                line->x, line->y, line->z);
                }
                else
                {
                    leaveWorld(kept, "where she stands (no zone line toward it from here)");
                }
            }
            else
            {
                leaveWorld(kept, "as she was, with no body,");
            }
            tell(PPlayer->id, setsOut.empty() ? fmt::format("{} sets out: {}.", nameOf(charid), title) : setsOut);
            return CL_S_OK;
        }

        auto sendQuest(CCharEntity* PPlayer, Record r, const cl_send_errand& ask) -> uint16
        {
            const auto goals = goalsOf(PPlayer);
            if (!goals.has_value())
            {
                return CL_S_REFUSED;
            }
            const bool  mission = ask.goal == CL_GOAL_MISSION;
            const Goal* goal    = nullptr;
            for (const auto& g : *goals)
            {
                if (g.mission == mission && g.log == ask.log && g.id == ask.id)
                {
                    goal = &g;
                }
            }
            if (goal == nullptr || ask.goal != CL_GOAL_QUEST)
            {
                return CL_S_NOT_OFFERED;
            }
            const auto log = logOf(r.charid);
            if (!log.has_value())
            {
                return CL_S_REFUSED;
            }
            if (!eligible(*log, *goal))
            {
                return doneAlready(*log, *goal) ? CL_S_DONE_ALREADY : CL_S_NOT_OFFERED;
            }
            if (const auto* PPawn = pawn::findPawn(r.charid); PPawn != nullptr && PPawn->isDead())
            {
                return CL_S_KNOCKED_OUT;
            }
            r.args["goal"]    = std::to_string(ask.goal);
            r.args["log"]     = std::to_string(goal->log);
            r.args["id"]      = std::to_string(goal->id);
            r.args["minutes"] = std::to_string(goal->minutes);
            if (goal->job.has_value())
            {
                r.args["job"] = std::to_string(*goal->job);
            }
            r.args["route"]  = cardian::errand::routeText(goal->route);
            r.errand.route   = goal->route;
            r.title          = goal->title;
            return setOut(PPlayer, std::move(r));
        }

        // Her nation's missions to the rank he picked: at most his own, as far
        // as the ladder goes; out of his party and to her zone line, as a quest
        auto sendRank(CCharEntity* PPlayer, Record r, const cl_send_errand& ask) -> uint16
        {
            const auto log = logOf(r.charid);
            if (!log.has_value())
            {
                return CL_S_REFUSED;
            }
            const auto now = rankOf(r.charid);
            if (ask.rank > rankCap(now.nation, PPlayer))
            {
                return CL_S_NOT_OFFERED;
            }
            const auto plan = rankPlanFor(*log, now.rank, ask.rank);
            if (!plan.has_value())
            {
                return CL_S_NOT_OFFERED;
            }
            if (const auto* PPawn = pawn::findPawn(r.charid); PPawn != nullptr && PPawn->isDead())
            {
                return CL_S_KNOCKED_OUT;
            }
            uint32                minutes = 0;
            std::vector<uint16_t> grants;
            for (std::size_t i = 0; i < plan->missions.size(); ++i)
            {
                minutes += plan->minutes[i];
                grants.push_back(plan->grants[i]);
            }
            r.args["nation"]   = std::to_string(now.nation);
            r.args["rank"]     = std::to_string(ask.rank);
            r.args["missions"] = cardian::errand::numbersText(plan->missions);
            r.args["mins"]     = cardian::errand::numbersText(plan->minutes);
            r.args["grants"]   = cardian::errand::numbersText(grants);
            r.args["minutes"]  = std::to_string(minutes);
            r.args["route"]    = cardian::errand::routeText(plan->route);
            r.errand.route     = plan->route;
            r.title            = fmt::format("Rank {}", ask.rank);
            return setOut(PPlayer, std::move(r));
        }

        // A money venture at a profession of hers (professions.h): fishing, at
        // a spot of the venture keeper's, with a bait of the catalog, for 1 or
        // 2 hours of the game clock. Out of his party and to her zone line
        // toward the spot, as a quest; the keeper works it once she is away
        auto sendMoney(CCharEntity* PPlayer, Record r, const cl_send_errand& ask) -> uint16
        {
            if (ask.profession != CL_PROF_FISHING)
            {
                return CL_S_NOT_OFFERED;
            }
            if (!pawn::professions::hasProfession(r.charid, ask.profession))
            {
                return CL_S_NO_PROFESSION;
            }
            if (!cardian::errand::ventureHours(ask.hours))
            {
                return CL_S_NOT_OFFERED;
            }
            const auto spot = pawn::professions::spotOf(r.charid, ask.profession, ask.zone, ask.area);
            if (!spot.has_value())
            {
                return CL_S_NOT_SAFE;
            }
            if (!pawn::professions::isBait(ask.bait) || !pawn::professions::rodTakeable(r.charid, ask.rod))
            {
                return CL_S_NOT_A_TOOL;
            }
            if (!pawn::professions::keeperAlive())
            {
                return CL_S_KEEPER_DOWN;
            }
            if (const auto* PPawn = pawn::findPawn(r.charid); PPawn != nullptr && PPawn->isDead())
            {
                return CL_S_KNOCKED_OUT;
            }
            r.args["profession"] = std::to_string(ask.profession);
            r.args["zone"]       = std::to_string(ask.zone);
            r.args["area"]       = std::to_string(ask.area);
            r.args["bait"]       = std::to_string(ask.bait);
            r.args["rod"]        = std::to_string(ask.rod);
            r.args["hours"]      = std::to_string(ask.hours);
            r.args["minutes"]    = std::to_string(static_cast<uint32>(ask.hours) * 60);
            r.args["placed"]     = "0";
            r.args["route"]      = cardian::errand::routeText({ ask.zone });
            r.errand.route       = { ask.zone };
            std::string zone; // her zone's name, as the game names it
            if (auto* PZone = zoneutils::GetZone(static_cast<xi::ZoneId>(ask.zone)); PZone != nullptr)
            {
                zone = PZone->getName();
                std::ranges::replace(zone, '_', ' ');
            }
            r.title         = zone;
            const auto line = fmt::format("{} sets out to fish in {}.", nameOf(r.charid), zone);
            pawn::professions::saveSet(r.charid, ask.profession, ask.rod, ask.bait);
            return setOut(PPlayer, std::move(r), line);
        }

        void sendErrand(CCharEntity* PChar, const cl_send_errand& ask, Reply& reply)
        {
            const auto member = pawn::club::memberOf(PChar, ask.cardian);
            if (!member.has_value())
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (records.contains(ask.cardian))
            {
                reply.finish(ask, CL_S_ON_ERRAND);
                return;
            }
            const auto kind = static_cast<Kind>(ask.errand);
            if (!cardian::errand::offered(kind, *member))
            {
                reply.finish(ask, CL_S_NOT_OFFERED);
                return;
            }
            Record r;
            r.charid         = ask.cardian;
            r.accid          = pawn::ownerAccountOf(PChar);
            r.playerCharID   = PChar->id;
            r.errand.kind    = kind;
            r.errand.state   = State::Going;
            r.errand.started = gameNow();
            reply.finish(ask, kind == Kind::Gear    ? sendGear(PChar, std::move(r))
                              : kind == Kind::Rank  ? sendRank(PChar, std::move(r), ask)
                              : kind == Kind::Money ? sendMoney(PChar, std::move(r), ask)
                                                    : sendQuest(PChar, std::move(r), ask));
        }

        void callBack(CCharEntity* PChar, const cl_call_back& ask, Reply& reply)
        {
            if (!pawn::club::memberOf(PChar, ask.cardian).has_value())
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            const auto it = records.find(ask.cardian);
            if (it == records.end())
            {
                reply.finish(ask, CL_S_NO_ERRAND);
                return;
            }
            ShowInfoFmt("errands: {} calls {} back", PChar->getName(), nameOf(ask.cardian));
            if (it->second.errand.kind == Kind::Money && cardian::errand::gone(it->second.errand.state))
            {
                if (it->second.errand.state == State::Away)
                {
                    returning(it->second, true);
                }
                reply.finish(ask, CL_S_OK);
                return;
            }
            const auto kept = cardian::errand::keptOnCallBack(it->second.errand, gameNow());
            finish(ask.cardian, kept, true);
            reply.finish(ask, CL_S_OK);
        }

        // The quests and missions he could send her on: the table's entries
        // he has done that she can do
        void errandGoals(CCharEntity* PChar, const cl_errands& ask, Reply& reply)
        {
            if (!pawn::club::memberOf(PChar, ask.cardian).has_value())
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            const auto goals = goalsOf(PChar);
            const auto log   = logOf(ask.cardian);
            if (!goals.has_value() || !log.has_value())
            {
                reply.finish(ask, CL_S_REFUSED);
                return;
            }
            auto answer  = ask;
            answer.count = 0;
            for (const auto& goal : *goals)
            {
                if (!eligible(*log, goal) || answer.count == UINT8_MAX)
                {
                    continue;
                }
                auto msg    = make<cl_errand_goal>();
                msg.goal    = goal.mission ? CL_GOAL_MISSION : CL_GOAL_QUEST;
                msg.log     = goal.log;
                msg.id      = goal.id;
                msg.minutes = goal.minutes;
                msg.unlocks = goal.job.has_value() ? 1 : 0;
                msg.job     = goal.job.value_or(0);
                setText(msg.title, goal.title);
                reply.more(msg);
                ++answer.count;
            }
            // the ranks a catch-up could take her to, each with its time
            const auto now = rankOf(ask.cardian);
            const auto cap = rankCap(now.nation, PChar);
            for (uint8 target = static_cast<uint8>(now.rank + 1); target <= cap && answer.count < UINT8_MAX; ++target)
            {
                const auto plan = rankPlanFor(*log, now.rank, target);
                if (!plan.has_value())
                {
                    continue;
                }
                uint32 minutes = 0;
                for (const auto m : plan->minutes)
                {
                    minutes += m;
                }
                auto msg     = make<cl_errand_goal>();
                msg.goal     = CL_GOAL_MISSION;
                msg.log      = now.nation;
                msg.id       = plan->missions.back();
                msg.minutes  = static_cast<uint16_t>(std::min<uint32>(minutes, UINT16_MAX));
                msg.rank     = target;
                msg.missions = static_cast<uint8_t>(std::min<std::size_t>(plan->missions.size(), UINT8_MAX));
                setText(msg.title, fmt::format("Rank {}", target));
                reply.more(msg);
                ++answer.count;
            }
            reply.finish(answer, CL_S_OK);
        }
    } // namespace

    // cardian_errands, a row an errand: made here, read at boot. A row is
    // written as she is sent (state 'going', `started` the game clock then),
    // written again as she leaves the world ('away', `left_at` and `ends`),
    // and deleted as she is back -- due, called back, or let go. `args` is
    // the kind's own (errand_math.h); `target` and `progress` are a kind's
    // that ends on its target rather than a clock
    void ensureTable()
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_errands` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`accid` int(10) unsigned NOT NULL, "
                         "`player_charid` int(10) unsigned NOT NULL, "
                         "`kind` varchar(16) NOT NULL, "
                         "`args` varchar(255) NOT NULL DEFAULT '', "
                         "`title` varchar(64) NOT NULL DEFAULT '', "
                         "`state` enum('going','away','returning','back') NOT NULL DEFAULT 'going', "
                         "`started` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`left_at` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`ends` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`target` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`progress` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`from_zone` smallint(5) unsigned NOT NULL DEFAULT '0', "
                         "`from_x` float NOT NULL DEFAULT '0', "
                         "`from_y` float NOT NULL DEFAULT '0', "
                         "`from_z` float NOT NULL DEFAULT '0', "
                         "`from_rot` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "PRIMARY KEY (`charid`), KEY `accid` (`accid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        // an older table: a money venture's returning and back
        db::preparedStmt("ALTER TABLE `cardian_errands` MODIFY `state` enum('going','away','returning','back') NOT NULL DEFAULT 'going'");
    }

    // Every row into memory. A walk off was the map's and ended with it: an
    // errand that was setting out leaves the world now (a quest), or is over
    // (gearing up in town)
    void load()
    {
        records.clear();
        const auto rset = db::preparedStmt("SELECT charid, accid, player_charid, kind, args, title, state, started, left_at, ends, target, progress, "
                                           "from_zone, from_x, from_y, from_z, from_rot FROM cardian_errands");
        std::vector<uint32> over;
        std::vector<uint32> leaving;
        while (rset && rset->next())
        {
            Record r;
            r.charid       = rset->get<uint32>("charid");
            r.accid        = rset->get<uint32>("accid");
            r.playerCharID = rset->get<uint32>("player_charid");
            const auto kind  = cardian::errand::kindOf(rset->get<std::string>("kind"));
            const auto state = cardian::errand::stateOf(rset->get<std::string>("state"));
            if (!kind.has_value() || !state.has_value())
            {
                ShowWarningFmt("errands: the row of {} does not read (kind '{}', state '{}'); it is let go", r.charid, rset->get<std::string>("kind"),
                               rset->get<std::string>("state"));
                over.push_back(r.charid);
                continue;
            }
            r.args            = cardian::errand::parseArgs(rset->get<std::string>("args"));
            r.title           = rset->get<std::string>("title");
            r.errand.kind     = *kind;
            r.errand.state    = *state;
            r.errand.started  = rset->get<uint32>("started");
            r.errand.left     = rset->get<uint32>("left_at");
            r.errand.ends     = rset->get<uint32>("ends");
            r.errand.target   = rset->get<uint32>("target");
            r.errand.progress = rset->get<uint32>("progress");
            r.errand.route    = cardian::errand::routeOf(r.args.contains("route") ? r.args.at("route") : std::string{});
            r.fromZone        = rset->get<uint16>("from_zone");
            r.from            = position_t(rset->get<float>("from_x"), rset->get<float>("from_y"), rset->get<float>("from_z"), 0, rset->get<uint8>("from_rot"));
            if (r.errand.state == State::Going)
            {
                (r.errand.kind == Kind::Gear ? over : leaving).push_back(r.charid);
            }
            records[r.charid] = std::move(r);
        }
        for (const auto charid : over)
        {
            if (records.contains(charid))
            {
                records.erase(charid);
            }
            db::preparedStmt("DELETE FROM cardian_errands WHERE charid = ?", charid);
        }
        for (const auto charid : leaving)
        {
            auto& r     = records[charid];
            const auto minutes = cardian::errand::numberArg(r.args, "minutes").value_or(60);
            r.errand    = cardian::errand::goAway(r.errand, gameNow(), minutes * 60);
            save(r);
        }
        ShowInfoFmt("errands: {} under way", records.size());
    }

    void registerHandlers()
    {
        cardian::link::handle<cl_errands>(errandGoals);
        cardian::link::handle<cl_send_errand>(sendErrand);
        cardian::link::handle<cl_call_back>(callBack);
    }

    void tick()
    {
        const auto now = timer::now();
        if (now - tickedAt < kTickEvery || records.empty())
        {
            return;
        }
        tickedAt = now;
        const uint32 clock = gameNow();

        std::vector<uint32> charids;
        charids.reserve(records.size());
        for (const auto& [charid, r] : records)
        {
            charids.push_back(charid);
        }
        for (const auto charid : charids)
        {
            const auto it = records.find(charid);
            if (it == records.end())
            {
                continue;
            }
            auto& r = it->second;
            if (r.errand.kind == Kind::Money && cardian::errand::gone(r.errand.state))
            {
                tickMoney(r, clock);
                continue;
            }
            if (r.errand.state == State::Away)
            {
                if (cardian::errand::due(r.errand, clock))
                {
                    finish(charid, cardian::errand::Kept::Whole, false);
                }
                continue;
            }
            if (r.errand.kind == Kind::Gear)
            {
                tickGear(r);
                continue;
            }
            // Setting out: at her zone line, over it, out of the world
            // already, or past her time to reach it, she leaves the world
            auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr || PPawn->loc.zone == nullptr)
            {
                leaveWorld(r, "(her body was taken on the way)");
                continue;
            }
            if (PPawn->isDead())
            {
                const auto   name   = PPawn->getName();
                const uint32 player = r.playerCharID;
                ShowInfoFmt("errands: {} fell on her way out; the errand is called off", name);
                finish(charid, cardian::errand::Kept::Nothing, true, false);
                tell(player, fmt::format("{} is KO'd, and does not set out.", name));
                continue;
            }
            const bool there   = distance(PPawn->loc.p, r.walk.there) <= r.walk.within;
            const bool crossed = static_cast<uint16>(PPawn->getZone()) != r.fromZone;
            const bool late    = timer::now() - r.walk.since >= kLeaveWithin;
            if (there || crossed || late)
            {
                leaveWorld(r, there ? "at the zone line" : (crossed ? "over the zone line" : "where she stands (the zone line took too long)"));
                continue;
            }
            if (r.walk.goal.has_value() && !pawn::walkOrderOf(charid).has_value())
            {
                pawn::setWalkOrder(charid, *r.walk.goal, pawn::kErrandWalker);
            }
        }
    }

    auto isAway(const uint32 charid) -> bool
    {
        const auto it = records.find(charid);
        return it != records.end() && cardian::errand::gone(it->second.errand.state);
    }

    auto viewOf(const uint32 charid) -> std::optional<View>
    {
        const auto it = records.find(charid);
        if (it == records.end())
        {
            return std::nullopt;
        }
        const auto&  r     = it->second;
        const auto&  e     = r.errand;
        const uint32 clock = gameNow();
        View         view{ .kind         = e.kind,
                           .state        = e.state,
                           .secondsLeft  = cardian::errand::secondsLeft(e, clock),
                           .secondsTotal = e.state == State::Away && e.ends > e.left ? e.ends - e.left : 0,
                           .zone         = cardian::errand::gone(e.state) ? cardian::errand::legAt(e, clock) : static_cast<uint16>(0),
                           .title        = r.title };
        if (e.kind == Kind::Rank)
        {
            const auto missions = cardian::errand::numbersOf(r.args.contains("missions") ? r.args.at("missions") : std::string{});
            const auto minutes  = cardian::errand::numbersOf(r.args.contains("mins") ? r.args.at("mins") : std::string{});
            const auto away     = e.state == State::Away && clock > e.left ? static_cast<uint64_t>(clock - e.left) : 0;
            view.missions       = static_cast<uint8>(std::min<std::size_t>(missions.size(), UINT8_MAX));
            view.missionsDone   = static_cast<uint8>(std::min<std::size_t>(cardian::errand::missionsDone(minutes, away), view.missions));
        }
        return view;
    }

    auto rankOf(const uint32 charid) -> Rank
    {
        if (const auto* PChar = loadedChar(charid); PChar != nullptr)
        {
            const uint8 nation = std::min<uint8>(PChar->profile.nation, 2);
            return Rank{ nation, std::max<uint8>(PChar->profile.rank[nation], 1) };
        }
        const auto rset = db::preparedStmt("SELECT c.nation, p.rank_sandoria, p.rank_bastok, p.rank_windurst FROM chars c "
                                           "JOIN char_profile p ON p.charid = c.charid WHERE c.charid = ?",
                                           charid);
        if (!rset || !rset->next())
        {
            return {};
        }
        const uint8 nation = std::min<uint8>(rset->get<uint8>("nation"), 2);
        return Rank{ nation, std::max<uint8>(rset->get<uint8>(kRankColumns[nation]), 1) };
    }

    auto rankCap(const uint8 nation, const CCharEntity* PPlayer) -> uint8
    {
        if (PPlayer == nullptr)
        {
            return 0;
        }
        const uint8 his = PPlayer->profile.rank[std::min<uint8>(PPlayer->profile.nation, 2)];
        return cardian::club::rankCap(ladderOf(nation).missions, his);
    }

    auto gearRefusal(const uint32 charid) -> uint16
    {
        auto* PPawn = pawn::findPawn(charid);
        if (PPawn == nullptr || PPawn->loc.zone == nullptr)
        {
            return CL_S_NOT_STANDING;
        }
        if (PPawn->isDead())
        {
            return CL_S_KNOCKED_OUT;
        }
        if (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged())
        {
            return CL_S_IN_A_FIGHT;
        }
        if (!pawn::seats::isWorlds(charid))
        {
            return CL_S_NOT_ADVENTURER; // the census dresses the world's alone
        }
        if (counterIn(PPawn) == nullptr)
        {
            return CL_S_NO_AUCTION_HOUSE;
        }
        return CL_S_OK;
    }

    void drop(const uint32 charid, const char* why)
    {
        if (!records.contains(charid))
        {
            return;
        }
        ShowInfoFmt("errands: {}'s errand ends: {}", nameOf(charid), why);
        finish(charid, cardian::errand::Kept::Nothing, true, false);
    }

    void endForLogin(const uint32 charid)
    {
        const auto it = records.find(charid);
        if (it == records.end())
        {
            return;
        }
        const auto kind = it->second.errand.kind;
        const auto kept = kind == Kind::Money ? cardian::errand::Kept::Nothing : cardian::errand::keptOnCallBack(it->second.errand, gameNow());
        ShowInfoFmt("errands: {} is logged in as, and her errand ends", nameOf(charid));
        endedAtLogin[charid] = kind;
        finish(charid, kept, true, false);
    }

    void zonedIn(CCharEntity* PChar)
    {
        if (PChar == nullptr || pawn::isPawn(PChar) || PChar->PSession == nullptr)
        {
            return;
        }
        const auto it = endedAtLogin.find(PChar->id);
        if (it == endedAtLogin.end())
        {
            return;
        }
        const auto line = it->second == Kind::Money ? fmt::format("{}'s fishing venture is cancelled.", PChar->getName())
                                                    : fmt::format("{}'s venture is cancelled.", PChar->getName());
        endedAtLogin.erase(it);
        PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar, MESSAGE_SYSTEM_3, line);
    }
} // namespace pawn::errands
