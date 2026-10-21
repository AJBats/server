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

#include "angler.h"
#include "auction.h"

#include "cardian_link.h"
#include "club.h"
#include "club_math.h"
#include "gate_guards.h"
#include "link_api.h"
#include "pawn.h"
#include "pawn_travel.h"
#include "professions.h"
#include "redress.h"
#include "redress_math.h"
#include "seats.h"
#include "supplies.h"
#include "venture_spots.h"
#include "world.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"
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
#include <ctime>
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
        constexpr float kAtLine      = 4.0f;                      // from her zone line's spot (its ground, subzones.h lineEnd): she is at it
        constexpr float kAtSpot      = 2.0f;                      // from where she started: she is back
        constexpr float kLineSnap    = 12.0f;                     // a zone line's spot is snapped to the mesh within this much
        constexpr float kShortOf     = 2.5f;                      // a counter or a guard is walked to this far short, on her side
        constexpr auto  kLevelLook   = std::chrono::seconds(3);   // a level venture away: her rows and the census's count read this often

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
            bool                   catchingUp = false; // a money venture making up the time the map was down
            uint32                 castsSeen  = 0;     // her angler's casts as last counted (angler::casts)
            uint8                  levelNow   = 0;     // a level venture away: her level on the job, as last read
            double                 share      = 0.0;   // ... the share of her next level the census has counted, as last read
            timer::time_point      levelRead{};        // ... when they were read
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

        // ---- a level venture -------------------------------------------------------

        // The job a level venture levels
        auto levelJobOf(const Record& r) -> uint8
        {
            return static_cast<uint8>(cardian::errand::numberArg(r.args, "job").value_or(0));
        }

        // He has called her back: the census counts no more of her venture
        // from the real second he did (recalled_at, Unix time)
        auto recalled(const Record& r) -> bool
        {
            return r.args.contains("recalled") && r.args.at("recalled") == "1";
        }

        auto recalledAt(const Record& r) -> uint32
        {
            return cardian::errand::numberArg(r.args, "recalled_at").value_or(0);
        }

        // The real clock, as the census stamps its count (Unix seconds)
        auto realSeconds() -> uint32
        {
            return static_cast<uint32>(std::time(nullptr));
        }

        // The census's count of this venture as its row holds it (census.py
        // LevelVentures: the row is keyed by her and the game clock as she
        // was sent, so a row of an earlier venture is never read): the levels
        // banked and the share of the next, the pace in levels an hour, and
        // the real clock as it was last run on (0 before its first run).
        // Nothing before the census has looked at the venture
        struct Count
        {
            double progress  = 0.0;
            double rate      = 0.0;
            double countedAt = 0.0;
        };

        auto countOf(const Record& r) -> std::optional<Count>
        {
            const auto rset = db::preparedStmt("SELECT progress, rate, counted_at FROM cardian_venture WHERE charid = ? AND started = ?", r.charid,
                                               r.errand.started);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            return Count{ rset->get<double>("progress"), rset->get<double>("rate"), rset->get<double>("counted_at") };
        }

        // The share of her next level the census has counted for this
        // venture; nothing before the census has looked at it
        auto ventureShare(const Record& r) -> std::optional<double>
        {
            const auto count = countOf(r);
            if (!count.has_value())
            {
                return std::nullopt;
            }
            return std::clamp(count->progress, 0.0, 1.0);
        }

        // The census is writing a ding of the venture in her rows now
        auto ventureClaimed(const uint32 charid) -> bool
        {
            const auto rset = db::preparedStmt("SELECT charid FROM cardian_ding WHERE charid = ? AND state = 'claimed' AND note = ?", charid,
                                               std::string(cardian::redress::kVentureNote));
            return rset && rset->next();
        }

        // A ding of the venture planned, or being written in her rows
        auto ventureDingOpen(const uint32 charid) -> bool
        {
            const auto rset = db::preparedStmt("SELECT charid FROM cardian_ding WHERE charid = ? AND state IN ('planned', 'claimed') AND note = ?", charid,
                                               std::string(cardian::redress::kVentureNote));
            return rset && rset->next();
        }

        // The census clock's speed (cardian_pace_clock, the census's table):
        // 1 in play, raised by a test world to see days of pace in minutes
        auto censusSpeed() -> double
        {
            const auto rset = db::preparedStmt("SELECT speed FROM cardian_pace_clock WHERE id = 1");
            return rset && rset->next() ? std::max(0.0, rset->get<double>("speed")) : 1.0;
        }

        // Her level on the venture's job as her body or her rows hold it now;
        // 0 when they cannot be read
        auto levelNowOf(const Record& r, const uint8 job) -> uint8
        {
            const auto jobs = jobsOf(r.charid);
            return jobs.has_value() && job < std::size(jobs->levels) ? cardian::errand::levelOn(jobs->levels[job]) : 0;
        }

        // The time the census has not counted yet of her venture, up to the
        // real second `until`, added to its row -- while the game's time runs
        // on with the map off (cardian.CLOCK_RUNS_OFFLINE), as the census
        // makes that time up itself -- so the census plans the levels it banks
        // (census.py LevelVentures.step). The time is added to the row as it
        // stands, so a ding the census lands meanwhile keeps its levels off it,
        // and only while the row's counted_at is still the one read: a count
        // the census ran on meanwhile has counted that time already, and the
        // next look reads it again. Her count as it then stands; nothing
        // without a row
        auto settleCount(const Record& r, const uint32 until) -> std::optional<double>
        {
            const auto count = countOf(r);
            if (!count.has_value())
            {
                return std::nullopt;
            }
            const auto   job  = levelJobOf(r);
            const uint8  now  = job != 0 ? levelNowOf(r, job) : 0;
            const uint32 left = now != 0 && r.errand.target > now ? r.errand.target - now : 0;
            if (!settings::get<bool>("cardian.CLOCK_RUNS_OFFLINE") || count->countedAt <= 0.0 || static_cast<double>(until) <= count->countedAt)
            {
                return count->progress;
            }
            const double settled = cardian::errand::ventureCount(count->progress, count->rate, count->countedAt, until, censusSpeed(), left);
            const double added   = settled - std::max(0.0, count->progress);
            if (!(added > 0.0))
            {
                return count->progress;
            }
            const auto rset = db::preparedStmt("UPDATE cardian_venture SET progress = progress + ?, counted_at = ? WHERE charid = ? AND started = ? AND counted_at = ?",
                                               added, static_cast<double>(until), r.charid, r.errand.started, count->countedAt);
            if (rset && rset->rowsAffected() > 0)
            {
                ShowInfoFmt("errands: {}'s level venture counts the {} minutes the census had not counted yet: {:.2f} level(s) more", nameOf(r.charid),
                            static_cast<uint32>((static_cast<double>(until) - count->countedAt) / 60.0), added);
            }
            const auto after = countOf(r);
            return after.has_value() ? std::optional<double>(after->progress) : std::nullopt;
        }

        // The venture over: a ding of it still waiting for the map is taken
        // back -- what it had counted is the share the call back keeps -- and
        // the census's row of it goes
        void endVenture(const uint32 charid)
        {
            db::preparedStmt("UPDATE cardian_ding SET state = 'done', note = 'her venture ended', done_at = NOW() WHERE charid = ? AND state = 'planned' AND note = ?",
                             charid, std::string(cardian::redress::kVentureNote));
            db::preparedStmt("DELETE FROM cardian_venture WHERE charid = ?", charid);
        }

        // Her skills as her rows hold them, by skill id, in the tenths of a
        // level the game counts: what a level venture's report compares
        auto skillsInRows(const uint32 charid) -> std::map<uint16, uint16>
        {
            std::map<uint16, uint16> out;
            const auto               rset = db::preparedStmt("SELECT skillid, value FROM char_skills WHERE charid = ?", charid);
            while (rset && rset->next())
            {
                out[rset->get<uint16>("skillid")] = rset->get<uint16>("value");
            }
            return out;
        }

        // Conquest points into her own nation's column: on her body when she
        // has one, else in her rows
        void addPoints(const uint32 charid, const uint32 points)
        {
            if (points == 0)
            {
                return;
            }
            static constexpr const char* kColumns[] = { "sandoria_cp", "bastok_cp", "windurst_cp" };
            if (auto* PChar = loadedChar(charid); PChar != nullptr)
            {
                charutils::AddPoints(PChar, kColumns[std::min<uint8>(PChar->profile.nation, 2)], static_cast<int32>(points));
                return;
            }
            const auto  rset   = db::preparedStmt("SELECT nation FROM chars WHERE charid = ?", charid);
            const uint8 nation = rset && rset->next() ? std::min<uint8>(rset->get<uint8>("nation"), 2) : 0;
            db::preparedStmt(fmt::format("INSERT INTO char_points (charid, {0}) VALUES (?, ?) ON DUPLICATE KEY UPDATE {0} = LEAST({0} + VALUES({0}), 2147483647)",
                                         kColumns[nation]),
                             charid, points);
        }

        // Her experience toward her next level on a job: her body's when she
        // has one, else her rows'
        auto expOn(const uint32 charid, const uint8 job) -> uint32
        {
            if (job == 0 || job >= kJobColumns.size())
            {
                return 0;
            }
            if (const auto* PChar = loadedChar(charid); PChar != nullptr)
            {
                return PChar->jobs.exp[job];
            }
            const auto rset = db::preparedStmt(fmt::format("SELECT {} AS exp FROM char_exp WHERE charid = ?", kJobColumns[job]), charid);
            return rset && rset->next() ? rset->get<uint32>("exp") : 0;
        }

        // Called back mid-level: the share of her next level the census has
        // counted kept as experience on the job (errand_math.h keptExp), on
        // her body when she has one, else in her rows; what she has is never
        // lowered. His alt or his own cardian earns the conquest points of the
        // experience the venture gained her, as each level did (census.py
        // pay_venture_points); one of the world's is paid for her levels
        // alone, by the census (pay_level). The experience kept
        auto keepShare(const Record& r) -> uint32
        {
            const auto job   = levelJobOf(r);
            const auto share = ventureShare(r);
            const auto jobs  = jobsOf(r.charid);
            if (!share.has_value() || !jobs.has_value() || job == 0 || job >= kJobColumns.size())
            {
                return 0;
            }
            const uint8 level = cardian::errand::levelOn(jobs->levels[job]);
            if (level >= r.errand.target)
            {
                return 0;
            }
            const uint32 exp = cardian::errand::keptExp(*share, charutils::GetExpNEXTLevel(level));
            if (exp == 0)
            {
                return 0;
            }
            const uint32 had = expOn(r.charid, job);
            if (auto* PChar = loadedChar(r.charid); PChar != nullptr)
            {
                PChar->jobs.exp[job] = static_cast<uint16>(std::max<uint32>(PChar->jobs.exp[job], std::min<uint32>(exp, UINT16_MAX)));
                charutils::SaveCharExp(PChar, static_cast<xi::Job>(job));
            }
            else
            {
                db::preparedStmt(fmt::format("UPDATE char_exp SET {0} = GREATEST({0}, ?) WHERE charid = ? LIMIT 1", kJobColumns[job]), exp, r.charid);
            }
            if (!ofTheWorld(r.charid))
            {
                addPoints(r.charid, cardian::errand::venturePoints(cardian::errand::ventureGain(0, exp, had), false));
            }
            return exp;
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
                if (const auto hop = pawn::travel::nextHop(here, PPawn->loc.p, static_cast<xi::ZoneId>(zone)); hop.has_value())
                {
                    return snap(PPawn->loc.zone, hop->walkTo, kLineSnap);
                }
            }
            return std::nullopt;
        }

        // The zone line nearest her into any zone next to hers, snapped to
        // the mesh: a level venture's way out of the world, which goes to no
        // zone in particular
        auto nearestLine(const CCharEntity* PPawn) -> std::optional<position_t>
        {
            const auto                here = PPawn->getZone();
            std::optional<position_t> best;
            float                     bestAway = 0.0f;
            for (const auto& [zone, hops] : pawn::travel::hops({ here }))
            {
                if (hops != 1)
                {
                    continue;
                }
                const auto hop = pawn::travel::nextHop(here, PPawn->loc.p, static_cast<xi::ZoneId>(zone));
                if (!hop.has_value() || hop->rents)
                {
                    continue;
                }
                const auto at = snap(PPawn->loc.zone, hop->walkTo, kLineSnap);
                if (!at.has_value())
                {
                    continue;
                }
                const float away = distance(PPawn->loc.p, *at);
                if (!best.has_value() || away < bestAway)
                {
                    best     = at;
                    bestAway = away;
                }
            }
            return best;
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

        // A money venture has her out at her spot: her saved place moved there
        auto placedOut(const Record& r) -> bool
        {
            return r.args.contains("placed") && r.args.at("placed") == "1";
        }

        // What she wore in the ranged and ammo slots before her fishing kit
        // went on, kept in the row so a restart finds it
        auto wornOf(const Record& r) -> pawn::angler::Worn
        {
            return { .known  = r.args.contains("worn_ranged"),
                     .ranged = static_cast<uint16>(cardian::errand::numberArg(r.args, "worn_ranged").value_or(0)),
                     .ammo   = static_cast<uint16>(cardian::errand::numberArg(r.args, "worn_ammo").value_or(0)) };
        }

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
            ShowInfoFmt("errands: {} leaves the world {} for {} ({}){}", nameOf(r.charid), how, r.title,
                        minutes > 0 ? fmt::format("{} minutes on the game clock", minutes) : std::string("off the clock, until it is done"),
                        pawn::findPawn(r.charid) != nullptr ? "; the ladder has not taken her body" : "");
        }

        // ---- a money venture's time made up --------------------------------------

        // The map was down while she was away: the casts of that time are
        // made up as it comes back. Her row keeps the game clock her fishing
        // has covered ("fished"): the end of her last cast while the map
        // runs, and while the time it was down is made up, moved on a cast at
        // a time before the cast is made, so a crash mid-way loses one cast at
        // most and never makes up the same time twice. "made_up_from" is
        // where the making up began, for his log. A row with no mark yet (her
        // venture began before the mark was kept) makes up nothing; nor does
        // the debug venture
        auto castsOwed(const Record& r, const uint32 clock) -> uint32
        {
            const auto from = cardian::errand::numberArg(r.args, "fished");
            if (!from.has_value() || (r.args.contains("debug") && r.args.at("debug") == "1"))
            {
                return 0;
            }
            const auto until = std::min(clock, r.errand.ends);
            const auto per   = std::max<uint32>(1, settings::get<uint32>("pawn.VENTURE_CATCH_UP_CAST_SECONDS"));
            return until > *from ? (until - *from) / per : 0;
        }

        void markFished(Record& r, const uint32 at)
        {
            r.args["fished"] = std::to_string(at);
            save(r);
        }

        // His log told how much of her venture was made up, once the making
        // up is over or her venture ends during it
        void tellMadeUp(const Record& r)
        {
            const auto from = cardian::errand::numberArg(r.args, "made_up_from");
            const auto to   = cardian::errand::numberArg(r.args, "fished");
            if (!from.has_value() || !to.has_value() || *to <= *from)
            {
                return;
            }
            const auto minutes = std::min<uint32>((*to - *from) / 60, UINT16_MAX);
            ShowInfoFmt("errands: {} made up {} minutes of fishing for the time the map was down", nameOf(r.charid), minutes);
            if (minutes > 0)
            {
                pawn::professions::recordNews(r.charid, r.accid, CL_NEWS_CAUGHT_UP, 0, 0, static_cast<uint16>(minutes));
            }
        }

        // A tick's casts of the time owed, VENTURE_CATCH_UP_CASTS at most;
        // owed no more, she fishes on as she does while the map runs
        auto catchUp(Record& r, const uint32 clock) -> pawn::angler::Stop
        {
            const auto per  = std::max<uint32>(1, settings::get<uint32>("pawn.VENTURE_CATCH_UP_CAST_SECONDS"));
            const auto most = std::max<uint32>(1, settings::get<uint32>("pawn.VENTURE_CATCH_UP_CASTS"));
            auto       stop = pawn::angler::Stop::None;
            for (uint32 i = 0; i < most && stop == pawn::angler::Stop::None && castsOwed(r, clock) > 0; ++i)
            {
                markFished(r, cardian::errand::numberArg(r.args, "fished").value_or(clock) + per);
                stop = pawn::angler::castAtOnce(r.charid);
            }
            r.castsSeen = pawn::angler::casts(r.charid);
            if (stop == pawn::angler::Stop::None && castsOwed(r, clock) == 0)
            {
                r.catchingUp = false;
                tellMadeUp(r);
                r.args.erase(std::string("made_up_from"));
                save(r);
            }
            return stop;
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
            else if (r.errand.kind == Kind::Level)
            {
                // Away and called back, the share of her next level is kept
                // before the venture's ding still waiting and its count go
                const auto   job     = levelJobOf(r);
                const auto*  jobName = job < kJobNames.size() ? kJobNames[job] : "?";
                const uint32 exp     = kept != cardian::errand::Kept::Nothing && r.errand.state == State::Away ? keepShare(r) : 0;
                endVenture(charid);
                const auto  jobs  = jobsOf(charid);
                const uint8 level = jobs.has_value() && job < std::size(jobs->levels) ? cardian::errand::levelOn(jobs->levels[job]) : 0;
                if (r.errand.state == State::Going || level == 0)
                {
                    line = fmt::format("{} is recalled.", name);
                }
                else if (level >= r.errand.target)
                {
                    line = fmt::format("{} is back: {} {}.", name, jobName, level);
                }
                else
                {
                    line = fmt::format("{} is recalled at {} {}.", name, jobName, level);
                }
                ShowInfoFmt("errands: {}'s level venture ends at {} {} (target {}){}", name, jobName, level, r.errand.target,
                            exp > 0 ? fmt::format("; {} experience kept toward the next level", exp) : std::string{});
                // her report as the venture ended, unread; nothing to report for one that never left
                if (r.errand.state == State::Going)
                {
                    db::preparedStmt("DELETE FROM cardian_level_report WHERE charid = ?", charid);
                }
                else
                {
                    const uint32 toNext = level > 0 ? charutils::GetExpNEXTLevel(level) : 0;
                    const uint8  keptAt = toNext > 0 ? static_cast<uint8>(std::min<uint32>(99, exp * 100 / toNext)) : 0;
                    db::preparedStmt("UPDATE cardian_level_report SET to_level = ?, minutes = GREATEST(0, CAST(? AS SIGNED) - CAST(left_at AS SIGNED)) DIV 60, "
                                     "exp_kept = ?, kept_share = ?, skills_to = ?, phase = ?, seen = 0 WHERE charid = ?",
                                     level, gameNow(), exp, keptAt, cardian::errand::skillsText(skillsInRows(charid)),
                                     static_cast<uint8>(CL_VENTURE_DONE), charid);
                }
            }
            else if (r.errand.kind == Kind::Money)
            {
                // her fishing over (angler.h): her kit off, her body released
                if (placedOut(r))
                {
                    pawn::angler::stop(charid, wornOf(r));
                }
                tellMadeUp(r);
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
            const auto zone = static_cast<uint16>(cardian::errand::numberArg(r.args, "zone").value_or(0));
            const auto area = static_cast<uint16>(cardian::errand::numberArg(r.args, "area").value_or(0));
            const auto spot = pawn::venturespots::waterOf(zone, area);
            const auto at   = spot.has_value() ? spot->centre : position_t{};
            save(r); // where she left, kept before her saved place moves
            db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_x = ?, pos_y = ?, pos_z = ? WHERE charid = ?", zone, at.x, at.y, at.z, r.charid);
            r.args["placed"] = "1";
            save(r);
            pawn::seats::touch(r.charid);
            ShowInfoFmt("errands: {} is faded at {} (zone {}, area {}) while she works", nameOf(r.charid), r.title, zone, area);
        }

        // Where and with what she fishes: the row's choices, her spot's point
        auto tripOf(const Record& r) -> pawn::angler::Trip
        {
            const auto profession = static_cast<uint8>(cardian::errand::numberArg(r.args, "profession").value_or(0));
            const auto zone       = static_cast<uint16>(cardian::errand::numberArg(r.args, "zone").value_or(0));
            const auto area       = static_cast<uint16>(cardian::errand::numberArg(r.args, "area").value_or(0));
            const auto spot       = pawn::venturespots::waterOf(zone, area);
            return { .charid     = r.charid,
                     .accid      = r.accid,
                     .profession = profession,
                     .zone       = zone,
                     .area       = area,
                     .areaName   = spot.has_value() ? spot->name : std::string{},
                     .spot       = spot.has_value() ? spot->centre : position_t{},
                     .rod        = static_cast<uint16>(cardian::errand::numberArg(r.args, "rod").value_or(0)),
                     .bait       = static_cast<uint16>(cardian::errand::numberArg(r.args, "bait").value_or(0)),
                     .leftAt     = r.errand.left,
                     .minutes    = static_cast<uint32>(cardian::errand::numberArg(r.args, "minutes").value_or(0)),
                     .letItemsGo = r.args.contains("items") && r.args.at("items") == "go",
                     .endOnSnap  = r.args.contains("snap") && r.args.at("snap") == "end",
                     .turbo      = r.args.contains("debug") && r.args.at("debug") == "1" };
        }

        auto stopWhy(const pawn::angler::Stop stop) -> std::string_view
        {
            switch (stop)
            {
                case pawn::angler::Stop::RodBroke:
                    return "her rod broke, and she held no spare";
                case pawn::angler::Stop::LureLost:
                    return "her lure was lost to a snapped line, and her player chose that it end the venture";
                case pawn::angler::Stop::RodAndLure:
                    return "her rod broke and took her lure, and her player chose that a lost lure end the venture, or she held no spare rod";
                case pawn::angler::Stop::BagsFull:
                    return "her inventory is full";
                case pawn::angler::Stop::NoGil:
                    return "neither purse could buy her kit";
                case pawn::angler::Stop::CannotFish:
                    return "she could not fish there";
                default:
                    return "";
            }
        }

        // Away: placed at her spot once the ladder has taken her body, then
        // fishing (angler.h) until her time is up or something stops her. A
        // restart that finds her away makes up the time the map was down
        // before her venture can end (catchUp)
        void tickMoney(Record& r, const uint32 clock)
        {
            const auto charid = r.charid;
            if (!placedOut(r))
            {
                if (pawn::findPawn(charid) != nullptr)
                {
                    return; // the ladder has not taken her body yet
                }
                place(r);
            }
            const bool owed = r.catchingUp || (!pawn::angler::fishing(charid) && castsOwed(r, clock) > 0);
            if (!owed && cardian::errand::due(r.errand, clock))
            {
                finish(charid, cardian::errand::Kept::Whole, false);
                return;
            }
            auto stop = pawn::angler::Stop::None;
            if (!pawn::angler::fishing(charid))
            {
                // what she wore is kept before her kit goes on: a restart
                // between the two would take the kit for what she wore
                auto worn = wornOf(r);
                stop      = pawn::angler::start(tripOf(r), worn,
                                                [&](const pawn::angler::Worn& w)
                                                {
                                                    r.args["worn_ranged"] = std::to_string(w.ranged);
                                                    r.args["worn_ammo"]   = std::to_string(w.ammo);
                                                    save(r);
                                                });
                r.castsSeen = 0;
                if (stop == pawn::angler::Stop::None && !r.args.contains("fished"))
                {
                    markFished(r, clock);
                }
                else if (stop == pawn::angler::Stop::None && owed)
                {
                    r.catchingUp = true;
                    if (!r.args.contains("made_up_from"))
                    {
                        r.args["made_up_from"] = r.args.at("fished");
                        save(r);
                    }
                    ShowInfoFmt("errands: {} makes up {} casts of fishing for the time the map was down", nameOf(charid), castsOwed(r, clock));
                }
            }
            else if (r.catchingUp)
            {
                stop = catchUp(r, clock);
            }
            else
            {
                stop = pawn::angler::tick(charid);
                if (const auto casts = pawn::angler::casts(charid); casts != r.castsSeen)
                {
                    r.castsSeen = casts;
                    markFished(r, clock);
                }
            }
            if (stop != pawn::angler::Stop::None)
            {
                ShowInfoFmt("errands: {}'s fishing venture is cut short: {}", nameOf(charid), stopWhy(stop));
                finish(charid, cardian::errand::Kept::Whole, false);
            }
        }

        // ---- a level venture away -----------------------------------------------

        // Away, the census levels her (census.py LevelVentures). Her level on
        // the job and the census's count of her next level are read every few
        // seconds, for the page and to know when she is done: once her rows
        // reach the target -- only while no ding of the venture is being
        // written in her rows -- or, once he has called her back, once what
        // the venture banked has landed: the time the census had not counted
        // when he called is added to its count, and while the count holds a
        // whole level or a ding of it is planned or being written, she waits
        // for the census to land it (errand_math.h recallWaits). Finishing
        // her ends the record: `r` is not read after
        void tickLevel(Record& r)
        {
            const auto now = timer::now();
            if (now - r.levelRead < kLevelLook)
            {
                return;
            }
            r.levelRead    = now;
            const auto job = levelJobOf(r);
            if (const auto level = levelNowOf(r, job); level != 0)
            {
                r.levelNow = level;
            }
            r.share = ventureShare(r).value_or(0.0);
            if (ventureClaimed(r.charid))
            {
                return;
            }
            if (r.levelNow >= r.errand.target)
            {
                finish(r.charid, cardian::errand::Kept::Whole, false);
                return;
            }
            if (!recalled(r))
            {
                return;
            }
            if (const auto count = settleCount(r, recalledAt(r)); count.has_value())
            {
                r.share         = std::clamp(*count, 0.0, 1.0);
                const auto left = r.errand.target > r.levelNow ? r.errand.target - r.levelNow : 0u;
                if (cardian::errand::recallWaits(*count, left, ventureDingOpen(r.charid)))
                {
                    return;
                }
            }
            finish(r.charid, cardian::errand::Kept::Progress, true);
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
                const auto line = kept.errand.kind == Kind::Level ? nearestLine(PPawn) : lineToward(PPawn, kept.errand.route);
                if (line.has_value())
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
        // one of her waters (venture_spots.h), with any bait of the game, for
        // 1 or 2 hours of the game clock. Out of his party and to her zone
        // line toward the water, as a quest; the map fishes it once she is
        // away (angler.h)
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
            // hours 0: the dev profile's one-minute venture, casting its hour at once
            const bool debug = ask.hours == 0 && settings::get<bool>("pawn.VENTURE_DEBUG");
            if (!cardian::errand::ventureHours(ask.hours) && !debug)
            {
                return CL_S_NOT_OFFERED;
            }
            if (!pawn::venturespots::reaches(PPlayer, r.charid, ask.zone, ask.area))
            {
                return CL_S_NOT_SAFE;
            }
            if (!pawn::professions::isBait(ask.bait) || !pawn::professions::rodTakeable(r.charid, ask.rod))
            {
                return CL_S_NOT_A_TOOL;
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
            r.args["items"]      = ask.letItemsGo != 0 ? "go" : "keep";
            r.args["snap"]       = ask.endOnSnap != 0 ? "end" : "replace";
            r.args["hours"]      = std::to_string(ask.hours);
            r.args["minutes"]    = std::to_string(debug ? 1u : static_cast<uint32>(ask.hours) * 60);
            r.args["debug"]      = debug ? "1" : "0";
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
            pawn::professions::saveSet(r.charid, ask.profession, ask.rod, ask.bait, ask.letItemsGo != 0, ask.endOnSnap != 0);
            return setOut(PPlayer, std::move(r), line);
        }

        // A level on one of her unlocked jobs, above hers on it and at most
        // his highest on any of his jobs (errand_math.h levelFits). Out of his
        // party and to the nearest zone line, then off the clock: the census
        // levels her while she is away, until her rows reach it
        auto sendLevel(CCharEntity* PPlayer, Record r, const cl_send_errand& ask) -> uint16
        {
            const auto jobs = jobsOf(r.charid);
            if (!jobs.has_value())
            {
                return CL_S_REFUSED;
            }
            const uint8 job = ask.job;
            if (job >= kJobNames.size() || !cardian::errand::levelFits(jobs->unlocked, job, jobs->levels[job], ask.level, levelCap(PPlayer)))
            {
                return CL_S_NOT_OFFERED;
            }
            if (const auto* PPawn = pawn::findPawn(r.charid); PPawn != nullptr && PPawn->isDead())
            {
                return CL_S_KNOCKED_OUT;
            }
            const uint8  from    = cardian::errand::levelOn(jobs->levels[job]);
            const uint32 expFrom = expOn(r.charid, job);
            r.args["job"]        = std::to_string(job);
            r.args["from"]    = std::to_string(from);
            r.args["minutes"] = "0"; // off the clock
            r.errand.target   = ask.level;
            r.levelNow        = from;
            r.title           = fmt::format("{} to {}", kJobNames[job], ask.level);
            db::preparedStmt("DELETE FROM cardian_venture WHERE charid = ?", r.charid); // an earlier venture's count, never read again
            const auto   line   = fmt::format("{} sets out to reach {} level {}.", nameOf(r.charid), kJobNames[job], ask.level);
            const uint32 charid = r.charid;
            const uint32 target = ask.level;
            const auto   status = setOut(PPlayer, std::move(r), line);
            if (status == CL_S_OK)
            {
                // her report from now: the job, her level, her experience toward her next and her skills as she sets out
                db::preparedStmt("REPLACE INTO cardian_level_report (charid, job, from_level, to_level, target, left_at, minutes, exp_from, exp_kept, kept_share, "
                                 "skills_from, skills_to, phase, seen) VALUES (?, ?, ?, ?, ?, ?, 0, ?, 0, 0, ?, '', ?, 0)",
                                 charid, job, from, from, target, gameNow(), expFrom, cardian::errand::skillsText(skillsInRows(charid)),
                                 static_cast<uint8>(CL_VENTURE_WORKING));
            }
            return status;
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
                              : kind == Kind::Level ? sendLevel(PChar, std::move(r), ask)
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
            if (it->second.errand.kind == Kind::Level && cardian::errand::gone(it->second.errand.state))
            {
                // The census counts nothing more from now; what the venture
                // banked lands first, and she is back once it has -- at once
                // when nothing is left to land (tickLevel). A second call back
                // keeps the first one's second
                auto& r = it->second;
                if (!recalled(r))
                {
                    r.args["recalled"]    = "1";
                    r.args["recalled_at"] = std::to_string(realSeconds());
                    save(r);
                }
                r.levelRead = {};
                tickLevel(r);
                reply.finish(ask, CL_S_OK);
                return;
            }
            if (it->second.errand.kind == Kind::Money && cardian::errand::gone(it->second.errand.state))
            {
                it->second.args["recalled"] = "1";
                finish(ask.cardian, cardian::errand::Kept::Whole, true);
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

    auto counterIn(const CCharEntity* PPawn) -> const CBaseEntity*
    {
        if (PPawn == nullptr || PPawn->loc.zone == nullptr)
        {
            return nullptr;
        }
        const CBaseEntity* PNearest = nullptr;
        float              nearest  = 0.0f;
        for (const auto* PCounter : pawn::auction::countersIn(PPawn->loc.zone))
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
                         "`state` enum('going','away') NOT NULL DEFAULT 'going', "
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
        // A level venture's report (levelReportOf): one row a member, her
        // last level venture -- the job, her level as she set out and as she
        // came back, the level sent for, the game clock as she set out and
        // the minutes away, her experience toward her next level as she set
        // out, the experience kept on a call back and its hundredths, her
        // skills as she set out and as she came back (errand_math.h
        // skillsText), its phase and the furthest phase her player has read
        // it at
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_level_report` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`job` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`from_level` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`to_level` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`target` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`left_at` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`minutes` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`exp_from` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`exp_kept` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`kept_share` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`skills_from` text NOT NULL, "
                         "`skills_to` text NOT NULL, "
                         "`phase` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`seen` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "PRIMARY KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("ALTER TABLE `cardian_level_report` ADD COLUMN IF NOT EXISTS `exp_from` int(10) unsigned NOT NULL DEFAULT '0' AFTER `minutes`");
        // A level venture's count, the census's (census.py VENTURE_TABLE, which
        // the watcher makes too): a row per venture away, keyed by her and the
        // game clock as she was sent -- the job, her count toward her next
        // levels, her pace in levels an hour, and the real clock as the census
        // last ran her count on (it makes up the time the map was off)
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `cardian_venture` ("
                         "`charid` int(10) unsigned NOT NULL, "
                         "`started` int(10) unsigned NOT NULL DEFAULT '0', "
                         "`job` tinyint(3) unsigned NOT NULL DEFAULT '0', "
                         "`progress` double NOT NULL DEFAULT '0', "
                         "`rate` double NOT NULL DEFAULT '0', "
                         "`counted_at` double NOT NULL DEFAULT '0', "
                         "PRIMARY KEY (`charid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
        db::preparedStmt("ALTER TABLE `cardian_venture` ADD COLUMN IF NOT EXISTS `counted_at` double NOT NULL DEFAULT '0'");
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
            const auto kind      = cardian::errand::kindOf(rset->get<std::string>("kind"));
            const auto stateText = rset->get<std::string>("state");
            auto       state     = cardian::errand::stateOf(stateText);
            // a fishing venture the old venture keeper left on its way home:
            // over, and finished at the first tick as any venture is
            const bool keeperDone = !state.has_value() && kind == Kind::Money && (stateText == "returning" || stateText == "back");
            if (keeperDone)
            {
                state = State::Away;
            }
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
            r.errand.ends     = keeperDone ? 1 : rset->get<uint32>("ends");
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
            if (r.errand.kind == Kind::Level && cardian::errand::gone(r.errand.state))
            {
                tickLevel(r);
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
        if (e.kind == Kind::Level)
        {
            view.levelJob    = levelJobOf(r);
            view.levelFrom   = static_cast<uint8>(cardian::errand::numberArg(r.args, "from").value_or(0));
            view.levelNow    = std::max(r.levelNow, view.levelFrom);
            view.levelTarget = static_cast<uint8>(std::min<uint32>(e.target, UINT8_MAX));
            view.levelShare  = cardian::errand::gone(e.state) ? cardian::errand::sharePercent(r.share) : static_cast<uint8>(0);
        }
        return view;
    }

    auto levelReportOf(const uint32 charid) -> std::optional<LevelReport>
    {
        const auto rset = db::preparedStmt("SELECT job, from_level, to_level, target, left_at, minutes, exp_from, exp_kept, kept_share, skills_from, skills_to, "
                                           "phase, seen FROM cardian_level_report WHERE charid = ?",
                                           charid);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        LevelReport out;
        out.phase = rset->get<uint8>("phase");
        if (out.phase != CL_VENTURE_WORKING && rset->get<uint8>("seen") >= CL_VENTURE_DONE)
        {
            return std::nullopt; // read once she was back
        }
        out.job       = rset->get<uint8>("job");
        out.from      = rset->get<uint8>("from_level");
        out.target    = rset->get<uint8>("target");
        auto skillsTo = cardian::errand::skillsOfText(rset->get<std::string>("skills_to"));
        if (out.phase == CL_VENTURE_WORKING)
        {
            // away: her rows as the census writes them, its count and its pace
            const auto   leftAt = rset->get<uint32>("left_at");
            const uint32 now    = gameNow();
            out.minutes         = now > leftAt ? (now - leftAt) / 60 : 0;
            const auto jobs     = jobsOf(charid);
            out.now = jobs.has_value() && out.job < std::size(jobs->levels) ? std::max(out.from, cardian::errand::levelOn(jobs->levels[out.job])) : out.from;
            // the census's count of this venture, never an earlier one's
            const auto it    = records.find(charid);
            const auto count = it != records.end() && it->second.errand.kind == Kind::Level ? countOf(it->second) : std::nullopt;
            if (count.has_value())
            {
                out.share       = cardian::errand::sharePercent(std::clamp(count->progress, 0.0, 1.0));
                out.expPerHour  = cardian::errand::expPerHour(count->rate, charutils::GetExpNEXTLevel(out.now));
                out.nextMinutes = cardian::errand::minutesToNext(count->progress, count->rate);
            }
            skillsTo = skillsInRows(charid);
        }
        else
        {
            out.now     = rset->get<uint8>("to_level");
            out.minutes = rset->get<uint32>("minutes");
            out.share   = rset->get<uint8>("kept_share");
            out.expKept = rset->get<uint32>("exp_kept");
        }
        // the conquest points of what the venture gained her: for his alt or
        // his own cardian, the experience behind the levels risen and any
        // kept, less what she held toward her next as she set out; for one of
        // the world's, the census's points for her levels alone
        uint64 levelsExp = 0;
        for (uint8 level = std::max<uint8>(out.from, 1); level < out.now; ++level)
        {
            levelsExp += charutils::GetExpNEXTLevel(level);
        }
        out.points = ofTheWorld(charid) ? cardian::errand::venturePoints(levelsExp, true)
                                        : cardian::errand::venturePoints(cardian::errand::ventureGain(levelsExp, out.expKept, rset->get<uint32>("exp_from")), false);
        out.skills = cardian::errand::skillsRisen(cardian::errand::skillsOfText(rset->get<std::string>("skills_from")), skillsTo);
        db::preparedStmt("UPDATE cardian_level_report SET seen = GREATEST(seen, ?) WHERE charid = ?", out.phase, charid);
        return out;
    }

    auto levelVentureDone(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_level_report WHERE charid = ? AND phase = ? AND seen < ?", charid,
                                           static_cast<uint8>(CL_VENTURE_DONE), static_cast<uint8>(CL_VENTURE_DONE));
        return rset && rset->next();
    }

    auto hasLevelReport(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_level_report WHERE charid = ? AND (phase = ? OR seen < ?)", charid,
                                           static_cast<uint8>(CL_VENTURE_WORKING), static_cast<uint8>(CL_VENTURE_DONE));
        return rset && rset->next();
    }

    auto levelCap(const CCharEntity* PPlayer) -> uint8
    {
        uint8 highest = 0;
        if (PPlayer != nullptr)
        {
            for (std::size_t job = 1; job < MAX_JOBTYPE; ++job)
            {
                highest = std::max(highest, PPlayer->jobs.job[job]);
            }
        }
        return highest;
    }

    auto ofTheWorld(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM cardian_census WHERE charid = ? AND recruited = 0", charid);
        return rset && rset->next();
    }

    auto jobsOf(const uint32 charid) -> std::optional<Jobs>
    {
        Jobs jobs;
        if (const auto* PChar = loadedChar(charid); PChar != nullptr)
        {
            jobs.unlocked = PChar->jobs.unlocked;
            for (std::size_t job = 1; job < MAX_JOBTYPE && job < std::size(jobs.levels); ++job)
            {
                jobs.levels[job] = PChar->jobs.job[job];
            }
            return jobs;
        }
        const auto rset = db::preparedStmt("SELECT * FROM char_jobs WHERE charid = ?", charid);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        jobs.unlocked = rset->get<uint32>("unlocked");
        for (std::size_t job = 1; job < kJobColumns.size() && job < std::size(jobs.levels); ++job)
        {
            jobs.levels[job] = rset->get<uint8>(std::string(kJobColumns[job]));
        }
        return jobs;
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
        // a level venture keeps the share of her next level, as his call back does
        const auto kept = kind == Kind::Money   ? cardian::errand::Kept::Nothing
                          : kind == Kind::Level ? cardian::errand::Kept::Progress
                                                : cardian::errand::keptOnCallBack(it->second.errand, gameNow());
        if (kind == Kind::Level && ventureClaimed(charid))
        {
            ShowWarningFmt("errands: {} is logged in as while the census writes a level of her venture; the level may be lost", nameOf(charid));
        }
        // A login cannot wait for what a level venture banked to land: the
        // time the census had not counted is added to its count, the share of
        // her next level is kept, and any whole level in it is lost
        if (auto& r = it->second; kind == Kind::Level && cardian::errand::gone(r.errand.state))
        {
            const auto  job   = levelJobOf(r);
            const uint8 level = levelNowOf(r, job);
            const auto  count = settleCount(r, recalled(r) ? recalledAt(r) : realSeconds());
            const auto  lost  = count.has_value() && level != 0 && r.errand.target > level ? cardian::errand::wholeLevels(*count, r.errand.target - level) : 0u;
            if (lost > 0)
            {
                const auto* jobName = job < kJobNames.size() ? kJobNames[job] : "?";
                ShowWarningFmt("errands: {} is logged in as with {} level{} of her venture banked and not yet landed ({} {}{}); they are lost, the share of "
                               "her next level kept",
                               nameOf(charid), lost, lost == 1 ? "" : "s", jobName, level + 1,
                               lost > 1 ? fmt::format(" to {}", level + lost) : std::string{});
            }
        }
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
