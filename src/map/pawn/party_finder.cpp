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

#include "party_finder.h"

#include "pawn.h"
#include "pawn_items.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "enums/mission_log.h"
#include "enums/msg_std.h"
#include "enums/party_kind.h"
#include "lua/lua_base_entity.h"
#include "packets/s2c/0x009_message.h"
#include "packets/s2c/0x017_chat_std.h"
#include "packets/s2c/0x0dc_group_solicit_req.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/jailutils.h"
#include "utils/zoneutils.h"
#include "world.h"
#include "zone.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace pawn::finder
{
    namespace
    {
        // The game's own value for a log with no mission on it (CCharEntity's
        // constructor; the census tool writes the same): the nation logs and
        // Zilart 0xFFFF, Aht Urhgan and Wings 0, Promathia its chapter-1
        // marker 101
        auto noMission(const uint8 log) -> uint16
        {
            return log <= 3 ? 0xFFFF : log == static_cast<uint8>(MissionLog::CoP) ? 101 : 0;
        }

        // Her rows, joined once: the census, her character, and the party
        // memory between her and this player
        struct Facts
        {
            uint32      charid   = 0;
            std::string name;
            uint16      posZone  = 0;
            uint8       job      = 0;
            uint8       level    = 0;
            uint32      seed     = 0;
            uint8       race     = 0;
            uint8       nation   = 0;
            uint8       rank[3]  = { 1, 1, 1 };
            bool        partied  = false;
            uint32      affinity = 0;
            // Her mission log as the census placed it (chars.missions); a
            // nation log with nothing on it reads 0xFFFF, the game's own "none"
            missionlog_t missions[MAX_MISSIONAREA]{};
        };

        constexpr auto kFactsQuery = "SELECT c.charid, c.charname, c.pos_zone, c.missions, s.mjob, s.mlvl, x.seed, x.nation, l.race, "
                                     "p.rank_sandoria, p.rank_bastok, p.rank_windurst, "
                                     "CAST(m.last_partied IS NOT NULL AS UNSIGNED) AS partied, "
                                     "CAST(COALESCE(m.affinity, 0) AS UNSIGNED) AS affinity "
                                     "FROM cardian_census x "
                                     "JOIN chars c ON c.charid = x.charid "
                                     "JOIN char_stats s ON s.charid = x.charid "
                                     "JOIN char_profile p ON p.charid = x.charid "
                                     "JOIN char_look l ON l.charid = x.charid "
                                     "LEFT JOIN cardian_party_memory m ON m.pawn_charid = x.charid AND m.player_charid = ? "
                                     "WHERE x.recruited = 0 AND x.charid <> 0";

        auto readFacts(const auto& rset) -> Facts
        {
            Facts f;
            f.charid   = rset->template get<uint32>("charid");
            f.name     = rset->template get<std::string>("charname");
            f.posZone  = rset->template get<uint16>("pos_zone");
            f.job      = rset->template get<uint8>("mjob");
            f.level    = rset->template get<uint8>("mlvl");
            f.seed     = rset->template get<uint32>("seed");
            f.race     = rset->template get<uint8>("race");
            f.nation   = std::min<uint8>(rset->template get<uint8>("nation"), 2);
            f.rank[0]  = rset->template get<uint8>("rank_sandoria");
            f.rank[1]  = rset->template get<uint8>("rank_bastok");
            f.rank[2]  = rset->template get<uint8>("rank_windurst");
            f.partied  = rset->template get<uint32>("partied") != 0;
            f.affinity = rset->template get<uint32>("affinity");
            for (uint8 log = 0; log < MAX_MISSIONAREA; ++log)
            {
                f.missions[log].current = noMission(log);
            }
            if (!rset->isNull("missions"))
            {
                db::extractFromBlob(rset, "missions", f.missions);
            }
            return f;
        }

        auto factsOf(const uint32 playerCharID, const uint32 charid) -> std::optional<Facts>
        {
            const auto rset = db::preparedStmt(std::string(kFactsQuery) + " AND x.charid = ?", playerCharID, charid);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            return readFacts(rset);
        }

        // The shout's pool: a body with an open contract is held for her
        // player (his Your contract rows, never a shout's), his or another's
        constexpr auto kNotHeld = " AND NOT EXISTS (SELECT 1 FROM cardian_party_memory h WHERE h.pawn_charid = x.charid AND h.contract <> '')";

        auto allFacts(const uint32 playerCharID) -> std::vector<Facts>
        {
            std::vector<Facts> out;
            const auto         rset = db::preparedStmt(std::string(kFactsQuery) + kNotHeld, playerCharID);
            while (rset && rset->next())
            {
                out.push_back(readFacts(rset));
            }
            return out;
        }

        // Her zone now: her body's when she stands, her row's otherwise
        auto zoneOf(const CCharEntity* PPawn, const uint16 rowZone) -> CZone*
        {
            return PPawn != nullptr ? PPawn->loc.zone : zoneutils::GetZone(static_cast<xi::ZoneId>(rowZone));
        }

        auto levelOf(const CCharEntity* PPawn, const uint8 rowLevel) -> uint8
        {
            return PPawn != nullptr ? PPawn->GetMLevel() : rowLevel;
        }

        auto inReach(const CCharEntity* PPlayer, CZone* PHere) -> bool
        {
            return PHere != nullptr && (PHere == PPlayer->loc.zone || pawn::sameCity(PHere, PPlayer->loc.zone));
        }

        // A nation's own missions are for its own people: nobody from
        // another nation hears a shout for them
        auto hears(const Facts& f, const Goal& goal) -> bool
        {
            return goal.kind != Goal::Kind::Mission || goal.log > static_cast<uint8>(MissionLog::Windurst) || f.nation == goal.log;
        }

        // Nobody already in the player's party hears the player's shout, nor
        // a body on her way out of town: she would be gone before the invite
        auto available(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn) -> bool
        {
            const bool partyMate = PPawn != nullptr && PPlayer->PParty != nullptr && PPawn->PParty == PPlayer->PParty;
            return !partyMate && !pawn::world::isLeaving(f.charid);
        }

        auto stateOf(const CCharEntity* PPlayer, const CCharEntity* PPawn, const uint32 charid, CZone* PHere) -> std::string
        {
            if (PPawn != nullptr)
            {
                return PPawn->PParty != nullptr ? "busy" : (PHere == PPlayer->loc.zone ? "here" : "standing");
            }
            if (!pawn::seats::has(charid))
            {
                return "away";
            }
            return pawn::world::campLeaderOf(charid) != 0 ? "busy" : "faded";
        }

        constexpr const char* kNationNames[] = { "San d'Oria", "Bastok", "Windurst" };

        // One of her lines, by her seed: the same ask gets the same words
        auto pick(const uint32 seed, std::initializer_list<const char*> lines) -> std::string
        {
            return *(lines.begin() + seed % lines.size());
        }

        // The player's fame level in each nation, the game's own reckoning,
        // read once per ask
        struct Fame
        {
            uint8 byNation[3] = { 1, 1, 1 };
        };

        auto fameOf(const CCharEntity* PPlayer) -> Fame
        {
            Fame            fame;
            CLuaBaseEntity  lua(const_cast<CCharEntity*>(PPlayer));
            const xi::FameArea areas[3] = { xi::FameArea::Sandoria, xi::FameArea::Bastok, xi::FameArea::Windurst };
            for (size_t i = 0; i < 3; ++i)
            {
                fame.byNation[i] = std::max<uint8>(lua.getFameLevel(areas[i]), 1);
            }
            return fame;
        }

        // Where her log stands against the mission the player is on in that
        // log (the goal): done (a nation log's completion flag; Promathia's
        // "every id below current"), on it, free to take it (the one before
        // done, or the log's first), or behind. A standing body's log is
        // hers in memory; a faded one's is her row's
        auto missionFitOf(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn, const Goal& goal) -> MissionFit
        {
            if (goal.log >= MAX_MISSIONAREA)
            {
                return MissionFit::Free;
            }
            const auto&  hers   = PPawn != nullptr ? PPawn->m_missionLog[goal.log] : f.missions[goal.log];
            const uint16 wanted = PPlayer->m_missionLog[goal.log].current;
            const uint16 none   = noMission(goal.log);
            if (wanted == none)
            {
                return MissionFit::Free;
            }
            // Complete as the game reads it (CLuaBaseEntity::hasCompletedMission):
            // Promathia and ids past the flags by "below her current", the rest by flag
            const bool cop  = goal.log == static_cast<uint8>(MissionLog::CoP);
            const auto done = [&](const uint16 id) { return (cop || id >= 64) ? id < hers.current : hers.complete[id]; };
            if (done(wanted))
            {
                return MissionFit::Done;
            }
            if (hers.current != none && hers.current == wanted)
            {
                return MissionFit::On;
            }
            return wanted == 0 || done(wanted - 1) ? MissionFit::Free : MissionFit::Behind;
        }

        // What stops her whatever her mood: a party, a fight, a camp, the
        // level band, a mission she has not reached. nullopt when nothing does
        auto hardNo(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn, const Goal& goal, const MissionFit fit) -> std::optional<Answer>
        {
            if (PPawn != nullptr)
            {
                if (PPawn->PParty != nullptr)
                {
                    return Answer{ false, fit, "I'm with a party already." };
                }
                if (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged())
                {
                    return Answer{ false, fit, "I'm in the middle of something!" };
                }
            }
            else if (!pawn::seats::has(f.charid))
            {
                return Answer{ false, fit, "She's nowhere to be found." };
            }
            else if (pawn::world::campLeaderOf(f.charid) != 0)
            {
                return Answer{ false, fit, "I'm out camping with friends." };
            }

            const int band  = settings::get<uint8>("pawn.FINDER_BAND");
            const int mine  = PPlayer->GetMLevel();
            const int level = levelOf(PPawn, f.level);
            if (level < mine - band)
            {
                return Answer{ false, fit, pick(f.seed, { "I'd only slow you down.", "I'm not ready for what you're doing." }) };
            }
            if (level > mine + band)
            {
                return Answer{ false, fit, pick(f.seed, { "Come back when you've grown a little.", "You'd be a liability out there." }) };
            }
            if (fit == MissionFit::Behind)
            {
                return Answer{ false, fit, pick(f.seed, { "I'd like to help, but I'm not on that mission yet!", "I haven't gotten that far. Sorry!" }) };
            }
            return std::nullopt;
        }

        // Her disposition: who you are (fame in her nation, rank against
        // hers), who you are to her (partied before, affinity), against her
        // seeded threshold; a quest asks more of it than exp, a mission more
        // still; `mood` is a shout's roll on it
        auto softAnswer(const CCharEntity* PPlayer, const Facts& f, const Goal& goal, const Fame& fame, const int mood, const MissionFit fit) -> Answer
        {
            const uint8 level     = fame.byNation[f.nation];
            const int   rankDiff  = static_cast<int>(PPlayer->profile.rank[f.nation]) - static_cast<int>(f.rank[f.nation]);
            const int   onIt      = fit == MissionFit::On ? 25 : fit == MissionFit::Done ? 5 : 0;
            const int   score     = 30 + mood + level * 5 + std::clamp(rankDiff * 5, -10, 15) + (f.partied ? 15 : 0) + static_cast<int>(std::min<uint32>(f.affinity, 6)) * 5 + onIt;
            const int   asking    = goal.kind == Goal::Kind::Mission ? 20 : goal.kind == Goal::Kind::Quest ? 10 : 0;
            const int   threshold = 25 + asking + static_cast<int>(f.seed % 40);
            if (score < threshold)
            {
                if (level <= 1 && !f.partied)
                {
                    return { false, fit, pick(f.seed, { "Do I know you?", "I don't party with strangers.", "Maybe when I've heard of you." }) };
                }
                if (asking > 0 && !f.partied)
                {
                    return { false, fit, pick(f.seed, { "That's a big ask from someone I've never partied with.", "Let's do some exp first, then we'll talk." }) };
                }
                return { false, fit, pick(f.seed, { "Not today.", "I'll pass, thanks.", "Some other time." }) };
            }
            if (fit == MissionFit::On)
            {
                return { true, fit, pick(f.seed, { "I'm on that one too. Let's do it.", "I could use a hand with that one myself.", "Same mission? Then we're partners." }) };
            }
            if (f.affinity >= 3)
            {
                return { true, fit, pick(f.seed, { "Always, for you.", "You know I'm in." }) };
            }
            if (f.partied)
            {
                return { true, fit, pick(f.seed, { "We've adventured together before. Count me in.", "You again? Gladly." }) };
            }
            if (fit == MissionFit::Done)
            {
                return { true, fit, pick(f.seed, { "I've done that one. I know the way.", "Been there. I'll show you." }) };
            }
            if (level >= 4)
            {
                return { true, fit, fmt::format("I've heard your name around {}. Let's go.", kNationNames[f.nation]) };
            }
            if (rankDiff > 0)
            {
                return { true, fit, "Someone of your rank asks? Of course." };
            }
            return { true, fit, pick(f.seed, { "Sure, why not.", "I could use the company.", "Lead the way." }) };
        }

        auto judge(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn, const Goal& goal, const Fame& fame, const int mood = 0) -> Answer
        {
            const auto fit = goal.kind == Goal::Kind::Mission ? missionFitOf(PPlayer, f, PPawn, goal) : MissionFit::Free;
            if (const auto hard = hardNo(PPlayer, f, PPawn, goal, fit); hard.has_value())
            {
                return *hard;
            }
            return softAnswer(PPlayer, f, goal, fame, mood, fit);
        }

        // Is the goal one the player's own log holds
        auto goalHeld(const CCharEntity* PPlayer, const Goal& goal, std::string& why) -> bool
        {
            switch (goal.kind)
            {
                case Goal::Kind::Mission:
                    if ((goal.log > 4 && goal.log != 6) || PPlayer->m_missionLog[goal.log].current == noMission(goal.log))
                    {
                        why = "you are not on a mission there";
                        return false;
                    }
                    return true;
                case Goal::Kind::Quest:
                    if (goal.log > 10)
                    {
                        why = "no such quest log";
                        return false;
                    }
                    return true;
                default:
                    return true;
            }
        }

        // A yes the finder took from the shout: the solicit that follows is
        // hers to accept without a second ask, and her contract starts on
        // that yes (pawn charid -> the inviter and the goal)
        struct Consent
        {
            uint32            playerCharID = 0;
            Goal              goal;
            timer::time_point at; // a consent not answered within kConsentLifetime lapses
        };
        std::unordered_map<uint32, Consent> consented;
        constexpr auto                      kConsentLifetime = std::chrono::minutes(2);

        // Her contract while she is in the party (pawn charid -> the player
        // and the goal), and the pawns known to hold none, so a miss is not
        // re-read from the database on every exp grant
        std::unordered_map<uint32, Consent> contracts;
        std::unordered_set<uint32>          noContract;
        std::unordered_map<uint32, uint32>  expBank; // exp toward her next point of affinity

        struct Held
        {
            Shout             shout;
            timer::time_point madeAt;
        };
        std::unordered_map<uint32, Held>              shouts; // by the player's charid
        std::unordered_map<uint32, timer::time_point> shoutedAt;
        uint32                                        lastShoutId = 0;
        // When each body last heard a player's shout, by player: the friend
        // seat goes to the friend longest unheard
        std::unordered_map<uint32, std::unordered_map<uint32, timer::time_point>> lastHeard;

        constexpr auto kShoutLifetime = std::chrono::minutes(10);

        auto currentShout(const uint32 playerCharID) -> Held*
        {
            const auto it = shouts.find(playerCharID);
            if (it == shouts.end())
            {
                return nullptr;
            }
            if (timer::now() - it->second.madeAt > kShoutLifetime)
            {
                shouts.erase(it);
                return nullptr;
            }
            return &it->second;
        }

        auto shoutWaitMs(const uint32 playerCharID) -> uint32
        {
            const auto it = shoutedAt.find(playerCharID);
            if (it == shoutedAt.end())
            {
                return 0;
            }
            const auto ready = it->second + std::chrono::seconds(settings::get<uint32>("pawn.SHOUT_COOLDOWN"));
            const auto now   = timer::now();
            return now >= ready ? 0 : static_cast<uint32>(std::chrono::duration_cast<std::chrono::milliseconds>(ready - now).count());
        }

        auto partySize(const CCharEntity* PPlayer) -> size_t
        {
            return PPlayer->PParty != nullptr ? std::max<size_t>(1, PPlayer->PParty->GetMemberCountAcrossAllProcesses()) : 1;
        }

        auto partyFull(const CCharEntity* PPlayer) -> bool
        {
            return PPlayer->PParty != nullptr && PPlayer->PParty->IsFull();
        }

        // Her yes in the player's current shout, for this same goal
        auto shoutedYes(const CCharEntity* PPlayer, const std::string& name, const Goal& goal) -> const Responder*
        {
            const auto* held = currentShout(PPlayer->id);
            if (held == nullptr || !(held->shout.goal == goal))
            {
                return nullptr;
            }
            const auto it = std::ranges::find_if(held->shout.rows, [&](const Responder& r)
            {
                return r.c.answer.yes && r.c.name == name;
            });
            return it != held->shout.rows.end() ? &*it : nullptr;
        }

        // Payload fragments sized for one GP_SERV_COMMAND_CHAT_STD each: the
        // gear line's packing
        constexpr size_t kChunkLimit = 110;

        void packEntry(std::vector<std::string>& chunks, const std::string& entry)
        {
            if (chunks.empty() || chunks.back().size() + 1 + entry.size() > kChunkLimit)
            {
                chunks.push_back(entry);
            }
            else
            {
                chunks.back() += "," + entry;
            }
        }

        // A recruit under contract with this player, or nullptr
        auto contractFor(const uint32 charid, const uint32 playerCharID) -> const Consent*
        {
            if (noContract.contains(charid))
            {
                return nullptr;
            }
            auto it = contracts.find(charid);
            if (it == contracts.end())
            {
                // After a restart: her memory row with the player she is partied with
                const auto* PPawn   = pawn::findPawn(charid);
                const auto* PPlayer = PPawn != nullptr ? pawn::partyPlayer(PPawn) : nullptr;
                if (PPlayer == nullptr)
                {
                    return nullptr;
                }
                const auto rset = db::preparedStmt("SELECT contract FROM cardian_party_memory WHERE player_charid = ? AND pawn_charid = ? AND contract <> ''", PPlayer->id, charid);
                if (!rset || !rset->next())
                {
                    noContract.insert(charid);
                    return nullptr;
                }
                it = contracts.insert_or_assign(charid, Consent{ PPlayer->id, goalFrom(rset->get<std::string>("contract"), 0) }).first;
            }
            return it->second.playerCharID == playerCharID ? &it->second : nullptr;
        }
    } // namespace

    auto goalFrom(const std::string& kind, const int log) -> Goal
    {
        Goal goal;
        if (kind == "mission")
        {
            goal.kind = Goal::Kind::Mission;
        }
        else if (kind == "quest")
        {
            goal.kind = Goal::Kind::Quest;
        }
        goal.log = static_cast<uint8>(std::clamp(log, 0, 255));
        return goal;
    }

    auto kindName(const Goal& goal) -> const char*
    {
        return goal.kind == Goal::Kind::Mission ? "mission" : goal.kind == Goal::Kind::Quest ? "quest" : "exp";
    }

    auto candidates(const CCharEntity* PPlayer, const Goal& goal) -> std::vector<Candidate>
    {
        std::vector<Candidate> out;
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return out;
        }

        const auto fame = fameOf(PPlayer);
        for (const auto& f : allFacts(PPlayer->id))
        {
            const auto* PPawn = pawn::findPawn(f.charid);
            auto*       PHere = zoneOf(PPawn, f.posZone);
            if (!inReach(PPlayer, PHere) || !hears(f, goal) || !available(PPlayer, f, PPawn))
            {
                continue;
            }

            Candidate c;
            c.name     = f.name;
            c.job      = f.job;
            c.level    = levelOf(PPawn, f.level);
            c.affinity = f.affinity;
            c.race     = f.race;
            c.nation   = f.nation;
            c.rank     = f.rank[f.nation];
            c.zone     = PHere->getName();
            c.zoneId   = static_cast<uint16>(PHere->GetID());
            c.state    = stateOf(PPlayer, PPawn, f.charid, PHere);
            c.answer   = judge(PPlayer, f, PPawn, goal, fame);
            c.charid   = f.charid;
            c.friendly = f.partied || f.affinity > 0;
            out.push_back(std::move(c));
        }

        const auto rankOf = [](const Candidate& c)
        {
            return c.state == "here" ? 0 : c.state == "standing" ? 1 : c.state == "faded" ? 2 : 3;
        };
        std::ranges::sort(out, [&](const Candidate& a, const Candidate& b)
        {
            if (a.answer.yes != b.answer.yes)
            {
                return a.answer.yes;
            }
            if (rankOf(a) != rankOf(b))
            {
                return rankOf(a) < rankOf(b);
            }
            if (a.level != b.level)
            {
                return a.level > b.level;
            }
            return a.name < b.name;
        });
        return out;
    }

    auto shout(const CCharEntity* PPlayer, const Goal& goal, const bool again, std::string& why) -> const Shout*
    {
        why.clear();
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            why = "no such player";
            return nullptr;
        }
        auto* current = currentShout(PPlayer->id);
        if (!again && current != nullptr && current->shout.goal == goal)
        {
            current->shout.waitMs = shoutWaitMs(PPlayer->id);
            return &current->shout;
        }
        if (partyFull(PPlayer))
        {
            why = "your party is full";
            return nullptr;
        }
        if (!goalHeld(PPlayer, goal, why))
        {
            return nullptr;
        }
        if (const auto wait = shoutWaitMs(PPlayer->id); wait > 0)
        {
            ShowInfoFmt("pawn: {} shouts again too soon ({} s left)", PPlayer->getName(), (wait + 999) / 1000);
            if (current != nullptr)
            {
                current->shout.waitMs = wait;
                return &current->shout;
            }
            why = fmt::format("you can shout again in {} seconds", (wait + 999) / 1000);
            return nullptr;
        }

        // Everyone in reach the world holds, each with a mood for this shout
        std::vector<Candidate> willing;
        std::vector<Candidate> unwilling;
        std::vector<Candidate> notYet; // a mission she has not reached: the extra nos
        const auto             fame = fameOf(PPlayer);
        for (const auto& f : allFacts(PPlayer->id))
        {
            const auto* PPawn = pawn::findPawn(f.charid);
            auto*       PHere = zoneOf(PPawn, f.posZone);
            if (!inReach(PPlayer, PHere) || !hears(f, goal) || !available(PPlayer, f, PPawn) || (PPawn == nullptr && !pawn::seats::has(f.charid)))
            {
                continue;
            }
            Candidate c;
            c.name     = f.name;
            c.job      = f.job;
            c.level    = levelOf(PPawn, f.level);
            c.affinity = f.affinity;
            c.race     = f.race;
            c.nation   = f.nation;
            c.rank     = f.rank[f.nation];
            c.zone     = PHere->getName();
            c.zoneId   = static_cast<uint16>(PHere->GetID());
            c.state    = stateOf(PPlayer, PPawn, f.charid, PHere);
            c.answer   = judge(PPlayer, f, PPawn, goal, fame, xirand::GetRandomNumber(-10, 11));
            c.charid   = f.charid;
            c.friendly = f.partied || f.affinity > 0;
            (c.answer.yes ? willing : c.answer.fit == MissionFit::Behind ? notYet : unwilling).push_back(std::move(c));
        }

        // Enough yeses to fill the party when the crowd has them (the user,
        // 2026-09-13: one shout should be able to form a party; a re-shout
        // is for the picky), the rest of the eight a mix, then up to three
        // who have not reached the mission, all in one shuffled order
        constexpr size_t kResponders = 8;
        constexpr size_t kNotYet     = 3;
        const size_t     need        = std::min<size_t>(kResponders, 6 - std::min<size_t>(6, partySize(PPlayer)));
        std::ranges::shuffle(willing, xirand::rng());
        std::ranges::shuffle(unwilling, xirand::rng());
        std::ranges::shuffle(notYet, xirand::rng());

        // The friend seat (the user, 2026-09-14): one of the eight goes to a
        // friend in reach, the one longest unheard, so friendship shows on
        // every shout without crowding out new faces. She answers as she
        // would anyway, and a yes counts toward the party; empty when no
        // friend is in reach
        std::vector<Candidate> picked;
        auto&                  heard = lastHeard[PPlayer->id];
        std::vector<std::pair<std::vector<Candidate>*, size_t>> friends;
        for (auto* list : { &willing, &unwilling, &notYet })
        {
            for (size_t i = 0; i < list->size(); ++i)
            {
                if ((*list)[i].friendly)
                {
                    friends.emplace_back(list, i);
                }
            }
        }
        std::ranges::shuffle(friends, xirand::rng());
        const auto heardAt = [&](const std::pair<std::vector<Candidate>*, size_t>& ref)
        {
            const auto it = heard.find((*ref.first)[ref.second].charid);
            return it != heard.end() ? it->second : timer::time_point::min();
        };
        std::string seat = "empty";
        if (const auto oldest = std::ranges::min_element(friends, {}, heardAt); oldest != friends.end())
        {
            auto& [list, index] = *oldest;
            seat                = (*list)[index].name;
            picked.push_back(std::move((*list)[index]));
            list->erase(list->begin() + static_cast<std::ptrdiff_t>(index));
        }
        const size_t seatYes = !picked.empty() && picked.front().answer.yes ? 1 : 0;
        const size_t sure    = std::min(need - std::min(need, seatYes), willing.size());
        picked.insert(picked.end(), std::make_move_iterator(willing.begin()), std::make_move_iterator(willing.begin() + sure));
        std::vector<Candidate> rest;
        rest.insert(rest.end(), std::make_move_iterator(willing.begin() + sure), std::make_move_iterator(willing.end()));
        rest.insert(rest.end(), std::make_move_iterator(unwilling.begin()), std::make_move_iterator(unwilling.end()));
        std::ranges::shuffle(rest, xirand::rng());
        const size_t fill = std::min(kResponders - picked.size(), rest.size());
        picked.insert(picked.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.begin() + fill));
        const size_t extra = std::min(kNotYet, notYet.size());
        picked.insert(picked.end(), std::make_move_iterator(notYet.begin()), std::make_move_iterator(notYet.begin() + extra));
        std::ranges::shuffle(picked, xirand::rng());

        // Her timing: the first voice after a beat, then uneven gaps -- most
        // short, now and then a lull -- and each deliberates a while
        Held made;
        made.madeAt     = timer::now();
        made.shout.id   = ++lastShoutId;
        made.shout.goal = goal;
        uint32 at       = xirand::GetRandomNumber(700, 2200);
        for (auto& c : picked)
        {
            heard[c.charid] = made.madeAt;
            Responder r;
            r.c        = std::move(c);
            r.revealMs = at;
            r.decideMs = xirand::GetRandomNumber(1500, 5200);
            const bool lull = xirand::GetRandomNumber(0, 5) == 0;
            at += lull ? xirand::GetRandomNumber(2500, 5000) : xirand::GetRandomNumber(400, 2400);
            made.shout.rows.push_back(std::move(r));
        }
        const auto yes = std::ranges::count_if(made.shout.rows, [](const Responder& r) { return r.c.answer.yes; });
        ShowInfoFmt("pawn: {} shouts ({}, log {}): {} hear it, {} would come, {} needed to fill the party, friend seat: {}",
                    PPlayer->getName(), kindName(goal), goal.log, made.shout.rows.size(), yes, need, seat);
        if (!made.shout.rows.empty())
        {
            shoutedAt[PPlayer->id] = made.madeAt;
        }
        made.shout.waitMs = shoutWaitMs(PPlayer->id);
        auto [it, inserted] = shouts.insert_or_assign(PPlayer->id, std::move(made));
        return &it->second.shout;
    }

    auto statsLine(CCharEntity* PPawn) -> std::string
    {
        return fmt::format("{}:{} {}:{} {}:{} {}:{} {}:{} {}:{} {}:{} {}:{}",
                           PPawn->STR(), PPawn->getMod(xi::Mod::STR), PPawn->DEX(), PPawn->getMod(xi::Mod::DEX),
                           PPawn->VIT(), PPawn->getMod(xi::Mod::VIT), PPawn->AGI(), PPawn->getMod(xi::Mod::AGI),
                           PPawn->INT(), PPawn->getMod(xi::Mod::INT), PPawn->MND(), PPawn->getMod(xi::Mod::MND),
                           PPawn->CHR(), PPawn->getMod(xi::Mod::CHR), PPawn->ATT(SLOT_MAIN), PPawn->DEF());
    }

    void snapshot(CCharEntity* PPawn)
    {
        if (PPawn == nullptr)
        {
            return;
        }
        db::preparedStmt("INSERT INTO cardian_snapshot (charid, level, hp, maxhp, mp, maxmp, str_t, dex_t, vit_t, agi_t, int_t, mnd_t, chr_t, "
                         "str_b, dex_b, vit_b, agi_b, int_b, mnd_b, chr_b, att, def, taken_at) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NOW()) "
                         "ON DUPLICATE KEY UPDATE level = VALUES(level), hp = VALUES(hp), maxhp = VALUES(maxhp), mp = VALUES(mp), maxmp = VALUES(maxmp), "
                         "str_t = VALUES(str_t), dex_t = VALUES(dex_t), vit_t = VALUES(vit_t), agi_t = VALUES(agi_t), int_t = VALUES(int_t), mnd_t = VALUES(mnd_t), chr_t = VALUES(chr_t), "
                         "str_b = VALUES(str_b), dex_b = VALUES(dex_b), vit_b = VALUES(vit_b), agi_b = VALUES(agi_b), int_b = VALUES(int_b), mnd_b = VALUES(mnd_b), chr_b = VALUES(chr_b), "
                         "att = VALUES(att), def = VALUES(def), taken_at = NOW()",
                         PPawn->id, PPawn->GetMLevel(),
                         static_cast<uint32>(std::max(0, PPawn->health.hp)), static_cast<uint32>(std::max(0, PPawn->GetMaxHP())),
                         static_cast<uint32>(std::max(0, PPawn->health.mp)), static_cast<uint32>(std::max(0, PPawn->GetMaxMP())),
                         static_cast<int16>(PPawn->STR()), static_cast<int16>(PPawn->DEX()), static_cast<int16>(PPawn->VIT()), static_cast<int16>(PPawn->AGI()),
                         static_cast<int16>(PPawn->INT()), static_cast<int16>(PPawn->MND()), static_cast<int16>(PPawn->CHR()),
                         PPawn->getMod(xi::Mod::STR), PPawn->getMod(xi::Mod::DEX), PPawn->getMod(xi::Mod::VIT), PPawn->getMod(xi::Mod::AGI),
                         PPawn->getMod(xi::Mod::INT), PPawn->getMod(xi::Mod::MND), PPawn->getMod(xi::Mod::CHR),
                         PPawn->ATT(SLOT_MAIN), PPawn->DEF());
    }

    auto peek(const CCharEntity* PPlayer, const std::string& name) -> std::optional<Peek>
    {
        if (PPlayer == nullptr)
        {
            return std::nullopt;
        }
        const uint32 charid = charutils::getCharIdFromName(name);
        const auto   facts  = charid != 0 ? factsOf(PPlayer->id, charid) : std::nullopt;
        if (!facts.has_value())
        {
            return std::nullopt;
        }
        // Someone in his current shout, or held by his own open contract
        const auto* held       = currentShout(PPlayer->id);
        const bool  shouted    = held != nullptr && std::ranges::any_of(held->shout.rows, [&](const Responder& r) { return r.c.name == facts->name; });
        const auto  contracted = openContractOf(charid);
        if (!shouted && !(contracted.has_value() && contracted->playerCharID == PPlayer->id))
        {
            return std::nullopt;
        }
        Peek p;
        p.name     = facts->name;
        p.nation   = facts->nation;
        p.rank     = facts->rank[facts->nation];
        p.affinity = facts->affinity;
        if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
        {
            p.standing = true;
            p.job      = static_cast<uint8>(PPawn->GetMJob());
            p.level    = PPawn->GetMLevel();
            p.sjob     = static_cast<uint8>(PPawn->GetSJob());
            p.slvl     = PPawn->GetSLevel();
            p.rank     = PPawn->profile.rank[facts->nation];
            p.gear     = pawn::items::equipChunks(PPawn);
            p.hp       = static_cast<uint32>(std::max(0, PPawn->health.hp));
            p.maxhp    = static_cast<uint32>(std::max(0, PPawn->GetMaxHP()));
            p.mp       = static_cast<uint32>(std::max(0, PPawn->health.mp));
            p.maxmp    = static_cast<uint32>(std::max(0, PPawn->GetMaxMP()));
            p.stats    = statsLine(PPawn);
            return p;
        }
        p.job   = facts->job;
        p.level = facts->level;
        if (const auto rset = db::preparedStmt("SELECT sjob, slvl FROM char_stats WHERE charid = ?", charid); rset && rset->next())
        {
            p.sjob = rset->get<uint8>("sjob");
            p.slvl = rset->get<uint8>("slvl");
        }
        // Her last stand's numbers, at her level today: a ding since makes
        // them another woman's
        if (const auto rset = db::preparedStmt("SELECT hp, maxhp, mp, maxmp, str_t, dex_t, vit_t, agi_t, int_t, mnd_t, chr_t, "
                                               "str_b, dex_b, vit_b, agi_b, int_b, mnd_b, chr_b, att, def FROM cardian_snapshot WHERE charid = ? AND level = ?",
                                               charid, p.level);
            rset && rset->next())
        {
            p.hp    = rset->get<uint32>("hp");
            p.maxhp = rset->get<uint32>("maxhp");
            p.mp    = rset->get<uint32>("mp");
            p.maxmp = rset->get<uint32>("maxmp");
            std::string line;
            for (const auto* stat : { "str", "dex", "vit", "agi", "int", "mnd", "chr" })
            {
                line += fmt::format("{}:{} ", rset->get<int16>(fmt::format("{}_t", stat)), rset->get<int16>(fmt::format("{}_b", stat)));
            }
            line += fmt::format("{}:{}", rset->get<uint16>("att"), rset->get<uint16>("def"));
            p.stats = line;
        }
        const auto rset = db::preparedStmt("SELECT slot, itemid FROM cardian_wardrobe WHERE name = ? ORDER BY slot", facts->name);
        while (rset && rset->next())
        {
            packEntry(p.gear, fmt::format("{}:{}:0", rset->get<uint8>("slot"), rset->get<uint16>("itemid")));
        }
        return p;
    }

    // Only a yes is spoken for: a no cannot be invited. A shout that has
    // lapsed, or whose player has left the world, holds nobody and is dropped
    auto shoutedFor(const std::string& name) -> bool
    {
        const auto now = timer::now();
        for (auto it = shouts.begin(); it != shouts.end();)
        {
            if (now - it->second.madeAt > kShoutLifetime || zoneutils::GetChar(it->first) == nullptr)
            {
                it = shouts.erase(it);
                continue;
            }
            if (std::ranges::any_of(it->second.shout.rows, [&](const Responder& r) { return r.c.answer.yes && r.c.name == name; }))
            {
                return true;
            }
            ++it;
        }
        return false;
    }

    auto invite(CCharEntity* PPlayer, const std::string& name, const Goal& goal) -> std::string
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return "no such player";
        }
        if (PPlayer->PParty != nullptr && PPlayer->PParty->GetLeader() != PPlayer)
        {
            return "you are not the party leader";
        }
        if (partyFull(PPlayer))
        {
            return "your party is full";
        }
        // The list's own gate: an unrecruited census body, a yes in the
        // player's current shout for this goal, in the player's zone or
        // city, then what stops her whatever she said before. Her zone is
        // her body's when she stands and her row's when she is faded
        const uint32 charid = charutils::getCharIdFromName(name);
        const auto   facts  = charid != 0 ? factsOf(PPlayer->id, charid) : std::nullopt;
        if (!facts.has_value())
        {
            return "she is not one of the world's adventurers";
        }
        auto* PPawn = pawn::findPawn(charid);
        // Her open contract with him is her yes, to its own goal: no shout,
        // and nothing asked again of what she agreed to. From another zone
        // the club's rule holds her until gathered (gatherOrHold)
        const auto held  = openContractOf(charid);
        const auto asked = held.has_value() ? held->goal : goal;
        if (held.has_value() && held->playerCharID != PPlayer->id)
        {
            return "she is under contract with another player";
        }
        if (!held.has_value())
        {
            if (currentShout(PPlayer->id) == nullptr)
            {
                return "your shout has faded (a shout lives ten minutes); shout again";
            }
            if (shoutedYes(PPlayer, facts->name, goal) == nullptr)
            {
                return "she did not answer your shout for that";
            }
            if (!inReach(PPlayer, zoneOf(PPawn, facts->posZone)))
            {
                return "she is not in your city";
            }
            const auto fit = goal.kind == Goal::Kind::Mission ? missionFitOf(PPlayer, *facts, PPawn, goal) : MissionFit::Free;
            if (const auto hard = hardNo(PPlayer, *facts, PPawn, goal, fit); hard.has_value())
            {
                return hard->line;
            }
        }
        if (PPawn == nullptr)
        {
            if (const auto* why = standFaded(PPlayer, charid); why != nullptr)
            {
                return why;
            }
            PPawn = pawn::findPawn(charid);
        }
        if (PPawn == nullptr)
        {
            return "she cannot stand just now";
        }

        if (PPawn->isDead())
        {
            return "she is KO'd";
        }
        if (PPawn->PParty != nullptr)
        {
            return "she is in a party already";
        }
        if (PPawn->InvitePending.UniqueNo != 0)
        {
            return "she already has an invite";
        }

        consented[charid] = Consent{ PPlayer->id, asked, timer::now() };
        PPawn->InvitePending.UniqueNo = PPlayer->id;
        PPawn->InvitePending.ActIndex = PPlayer->targid;
        PPawn->pushPacket<GP_SERV_COMMAND_GROUP_SOLICIT_REQ>(PPawn->id, PPawn->targid, PPlayer->getName(), PartyKind::Party);
        ShowInfoFmt("pawn: {} invites {} from the party finder, for {}{}", PPlayer->getName(), PPawn->getName(), kindName(asked), held.has_value() ? ", her open contract" : "");
        return "";
    }

    auto accepts(CCharEntity* PPawn) -> bool
    {
        if (PPawn == nullptr)
        {
            return false;
        }
        const auto* PInviter = zoneutils::GetCharFromWorld(PPawn->InvitePending.UniqueNo, PPawn->InvitePending.ActIndex);
        // Held for her player while her contract is open: his invite alone
        // (by id: he may be mid-zone as she answers)
        if (const auto held = openContractOf(PPawn->id); held.has_value() && PPawn->InvitePending.UniqueNo != held->playerCharID)
        {
            ShowInfoFmt("pawn: {} declines {}'s invite: she is under contract with another player", PPawn->getName(), PInviter != nullptr ? PInviter->getName() : "someone");
            consented.erase(PPawn->id);
            return false;
        }
        const auto it   = consented.find(PPawn->id);
        const bool wild = pawn::seats::isWorlds(PPawn->id);
        if (it == consented.end())
        {
            if (!wild)
            {
                return true; // hers to accept: an alt or a recruit
            }
            ShowInfoFmt("pawn: {} declines {}'s invite: nobody shouted for her", PPawn->getName(), PInviter != nullptr ? PInviter->getName() : "someone");
            return false;
        }
        const Consent consent = it->second;
        consented.erase(it);
        if (PInviter == nullptr || PInviter->id != consent.playerCharID || timer::now() - consent.at > kConsentLifetime)
        {
            return !wild;
        }
        // Her contract starts on the yes, and the two of you have partied
        contracts[PPawn->id] = consent;
        noContract.erase(PPawn->id);
        db::preparedStmt("UPDATE cardian_party_memory SET contract = '' WHERE pawn_charid = ? AND player_charid <> ? AND contract <> ''", PPawn->id, consent.playerCharID);
        db::preparedStmt("INSERT INTO cardian_party_memory (player_charid, pawn_charid, last_partied, contract) VALUES (?, ?, NOW(), ?) "
                         "ON DUPLICATE KEY UPDATE contract = VALUES(contract), last_partied = NOW()",
                         consent.playerCharID, PPawn->id, kindName(consent.goal));
        ShowInfoFmt("pawn: {} joins {} under the {} contract", PPawn->getName(), PInviter->getName(), kindName(consent.goal));
        return true;
    }

    auto contractWith(const uint32 charid, const uint32 playerCharID) -> const char*
    {
        const auto* c = contractFor(charid, playerCharID);
        return c != nullptr ? kindName(c->goal) : "";
    }

    void noteLeft(const uint32 charid)
    {
        contracts.erase(charid);
        noContract.erase(charid);
        expBank.erase(charid);
        if (const auto held = openContractOf(charid); held.has_value())
        {
            db::preparedStmt("UPDATE cardian_party_memory SET contract = '' WHERE pawn_charid = ? AND contract <> ''", charid);
            ShowInfoFmt("pawn: {}'s {} contract with {} ends", held->name, kindName(held->goal), pawn::seats::nameOf(held->playerCharID));
        }
    }

    auto openContractOf(const uint32 charid) -> std::optional<OpenContract>
    {
        const auto rset = db::preparedStmt("SELECT m.player_charid, m.contract, c.charname FROM cardian_party_memory m JOIN chars c ON c.charid = m.pawn_charid "
                                           "JOIN cardian_census x ON x.charid = m.pawn_charid "
                                           "WHERE m.pawn_charid = ? AND m.contract <> '' AND x.recruited = 0 ORDER BY m.last_partied DESC LIMIT 1",
                                           charid);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        return OpenContract{ .charid       = charid,
                             .playerCharID = rset->get<uint32>("player_charid"),
                             .name         = rset->get<std::string>("charname"),
                             .goal         = goalFrom(rset->get<std::string>("contract"), 0) };
    }

    auto openContracts(const uint32 playerCharID) -> std::vector<OpenContract>
    {
        std::vector<OpenContract> out;
        const auto rset = db::preparedStmt("SELECT m.pawn_charid, m.contract, c.charname FROM cardian_party_memory m JOIN chars c ON c.charid = m.pawn_charid "
                                           "JOIN cardian_census x ON x.charid = m.pawn_charid "
                                           "WHERE m.player_charid = ? AND m.contract <> '' AND x.recruited = 0 ORDER BY c.charname",
                                           playerCharID);
        while (rset && rset->next())
        {
            out.push_back(OpenContract{ .charid       = rset->get<uint32>("pawn_charid"),
                                        .playerCharID = playerCharID,
                                        .name         = rset->get<std::string>("charname"),
                                        .goal         = goalFrom(rset->get<std::string>("contract"), 0) });
        }
        return out;
    }

    auto release(CCharEntity* PPlayer, const std::string& name) -> std::string
    {
        if (PPlayer == nullptr)
        {
            return "no such player";
        }
        const uint32 charid = charutils::getCharIdFromName(name);
        const auto   held   = charid != 0 ? openContractOf(charid) : std::nullopt;
        if (!held.has_value() || held->playerCharID != PPlayer->id)
        {
            return "she holds no contract with you";
        }
        if (const auto* PPawn = pawn::findPawn(charid); PPawn != nullptr && PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty)
        {
            return "she is in your party: the party is how she leaves it";
        }
        noteLeft(charid);
        pawn::forgetGuestGambits(charid);
        pawn::returnToWorld(charid, "released from her contract");
        return "";
    }

    void noteExp(const CCharEntity* PPawn, const uint32 exp)
    {
        if (PPawn == nullptr || exp == 0 || PPawn->PParty == nullptr)
        {
            return;
        }
        auto* PPlayer = pawn::partyPlayer(PPawn);
        if (PPlayer == nullptr)
        {
            return;
        }
        const auto* c = contractFor(PPawn->id, PPlayer->id);
        if (c == nullptr || c->goal.kind != Goal::Kind::Experience)
        {
            return;
        }
        const uint32 per  = std::max<uint32>(1, settings::get<uint32>("pawn.AFFINITY_EXP"));
        auto&        bank = expBank[PPawn->id];
        bank += exp;
        while (bank >= per)
        {
            bank -= per;
            bond(PPlayer->id, PPawn->id, "exp gained in the party");
            PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, fmt::format("{} thinks a little more of you.", PPawn->getName()));
        }
    }

    void bond(const uint32 playerCharID, const uint32 pawnCharID, const char* why, const bool mission)
    {
        if (playerCharID == 0 || pawnCharID == 0)
        {
            return;
        }
        const uint32 missions = mission ? 1 : 0;
        db::preparedStmt("INSERT INTO cardian_party_memory (player_charid, pawn_charid, last_partied, affinity, missions) VALUES (?, ?, NOW(), 1, ?) "
                         "ON DUPLICATE KEY UPDATE affinity = affinity + 1, missions = missions + ?, last_partied = NOW()",
                         playerCharID, pawnCharID, missions, missions);
        ShowInfoFmt("pawn: {}'s affinity with {} grows: {}{}", pawn::seats::nameOf(pawnCharID), pawn::seats::nameOf(playerCharID), why, mission ? " (a mission together)" : "");
    }

    auto standFaded(const CCharEntity* PPlayer, const uint32 charid) -> const char*
    {
        if (PPlayer == nullptr || charid == 0 || pawn::findPawn(charid) != nullptr)
        {
            return nullptr;
        }
        if (!pawn::seats::heldFor(charid, PPlayer->id))
        {
            return "she is away";
        }
        if (pawn::world::campLeaderOf(charid) != 0)
        {
            return "she is in a party already";
        }
        if (!pawn::seats::inviteStand(charid))
        {
            ShowInfoFmt("pawn: {} invites {}; she is faded and cannot stand now (nobody near her zone, the cap full of party members, or her stand waits on a retry)",
                        PPlayer->getName(), pawn::seats::nameOf(charid));
            return "she cannot stand just now";
        }
        const auto* PPawn = pawn::findPawn(charid);
        ShowInfoFmt("pawn: {} invites {}; she was faded and stands for it", PPlayer->getName(), PPawn != nullptr ? PPawn->getName() : pawn::seats::nameOf(charid));
        return PPawn != nullptr ? nullptr : "she cannot stand just now";
    }

    auto interceptInvite(CCharEntity* PPlayer, const uint32 charid) -> bool
    {
        if (PPlayer == nullptr || charid == 0 || !pawn::seats::has(charid))
        {
            return false;
        }
        if (jailutils::InPrison(PPlayer) || (PPlayer->PParty != nullptr && (PPlayer->PParty->GetLeader() != PPlayer || PPlayer->PParty->IsFull())))
        {
            return false;
        }
        if (pawn::seats::isWorlds(charid))
        {
            const auto name = pawn::seats::nameOf(charid);
            PPlayer->pushPacket<GP_SERV_COMMAND_MESSAGE>(PPlayer, 0, 0, MsgStd::InvitationDeclined);
            PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, fmt::format("{} is one of the world's adventurers. Recruit her through the Party Finder.", name));
            ShowInfoFmt("pawn: {} invites {} by name; refused, she is the world's (the Party Finder recruits her)", PPlayer->getName(), name);
            return true;
        }
        // The player's own, faded: stood so the handler finds her
        if (pawn::findPawn(charid) == nullptr)
        {
            if (const auto* why = standFaded(PPlayer, charid); why != nullptr && pawn::findPawn(charid) == nullptr)
            {
                ShowInfoFmt("pawn: {} invites {} by name; {}, so the game drops the invite", PPlayer->getName(), pawn::seats::nameOf(charid), why);
            }
        }
        return false;
    }
} // namespace pawn::finder
