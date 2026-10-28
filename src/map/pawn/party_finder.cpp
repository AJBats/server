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

#include "cardian_link_messages.h"
#include "club.h"
#include "pawn.h"
#include "pawn_travel.h"
#include "seats.h"
#include "teleport.h"

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
#include "items/item_equipment.h"
#include "lua/lua_base_entity.h"
#include "packets/s2c/0x009_message.h"
#include "packets/s2c/0x017_chat_std.h"
#include "packets/s2c/0x0dc_group_solicit_req.h"
#include "party.h"
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
            uint8       sjob     = 0;
            uint8       slvl     = 0;
            uint32      seed     = 0;
            uint8       race     = 0;
            uint8       nation   = 0;
            uint8       rank[3]  = { 1, 1, 1 };
            bool        partied  = false;
            uint32      affinity = 0;
            uint32      together = 0; // story missions completed with him (cardian_party_memory.missions)
            // Her mission log as the census placed it (chars.missions); a
            // nation log with nothing on it reads 0xFFFF, the game's own "none"
            missionlog_t missions[MAX_MISSIONAREA]{};
        };

        constexpr auto kFactsQuery = "SELECT c.charid, c.charname, c.pos_zone, c.missions, s.mjob, s.mlvl, s.sjob, s.slvl, x.seed, x.nation, l.race, "
                                     "p.rank_sandoria, p.rank_bastok, p.rank_windurst, "
                                     "CAST(m.last_partied IS NOT NULL AS UNSIGNED) AS partied, "
                                     "CAST(COALESCE(m.affinity, 0) AS UNSIGNED) AS affinity, "
                                     "CAST(COALESCE(m.missions, 0) AS UNSIGNED) AS together "
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
            f.sjob     = rset->template get<uint8>("sjob");
            f.slvl     = rset->template get<uint8>("slvl");
            f.seed     = rset->template get<uint32>("seed");
            f.race     = rset->template get<uint8>("race");
            f.nation   = std::min<uint8>(rset->template get<uint8>("nation"), 2);
            f.rank[0]  = rset->template get<uint8>("rank_sandoria");
            f.rank[1]  = rset->template get<uint8>("rank_bastok");
            f.rank[2]  = rset->template get<uint8>("rank_windurst");
            f.partied  = rset->template get<uint32>("partied") != 0;
            f.affinity = rset->template get<uint32>("affinity");
            f.together = rset->template get<uint32>("together");
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

        // The shout's pool: the online -- a session row, standing or faded,
        // so search finds her -- less a body with an open contract, held for
        // her player (his Your contract rows, never a shout's), his or another's,
        // or by a linkshell's pearl she wears (club.h: a pearl or a sack in a link slot)
        constexpr auto kShoutPool = " AND EXISTS (SELECT 1 FROM accounts_sessions o WHERE o.charid = x.charid)"
                                    " AND NOT EXISTS (SELECT 1 FROM cardian_party_memory h WHERE h.pawn_charid = x.charid AND h.contract <> '')"
                                    " AND NOT EXISTS (SELECT 1 FROM char_equip le JOIN char_inventory li ON li.charid = le.charid AND li.location = le.containerid AND li.slot = le.slotid"
                                    " WHERE le.charid = x.charid AND le.equipslotid IN (16, 17) AND li.itemId IN (514, 515))";

        auto allFacts(const uint32 playerCharID) -> std::vector<Facts>
        {
            std::vector<Facts> out;
            const auto         rset = db::preparedStmt(std::string(kFactsQuery) + kShoutPool, playerCharID);
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

        // A city or a town, by the game's own zone type (Selbina and Mhaura
        // are typed cities too): where the shout is heard
        auto cityOrTown(CZone* PZone) -> bool
        {
            return PZone != nullptr && (PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
        }

        // Nobody already in the player's party hears the player's shout, nor
        // a body on her way out of town: she would be gone before the invite
        auto available(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn) -> bool
        {
            const bool partyMate = PPawn != nullptr && PPlayer->PParty != nullptr && PPawn->PParty == PPlayer->PParty;
            return !partyMate && !pawn::world::isLeaving(f.charid);
        }

        auto presenceOf(const CCharEntity* PPlayer, const CCharEntity* PPawn, const uint32 charid, CZone* PHere) -> Presence
        {
            if (PPawn != nullptr)
            {
                return PPawn->PParty != nullptr ? Presence::Busy : (PHere == PPlayer->loc.zone ? Presence::Here : Presence::Standing);
            }
            if (!pawn::seats::has(charid))
            {
                return Presence::Away;
            }
            return pawn::world::campLeaderOf(charid) != 0 ? Presence::Busy : Presence::Faded;
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

        // A mission of several legs is one mission: the rank 2-3 mission of
        // each nation is its start and four legs, ids 5 to 9 of the nation's
        // log (the other two nations' first and second legs, in either
        // order). The census places a body's story by her level and all five
        // sit at level 25 (missions.yaml), so a body is on its start or past
        // all of it, never on a later leg: a leg is matched by its mission's
        // start
        auto missionStartOf(const uint8 log, const uint16 id) -> uint16
        {
            constexpr uint16 kLegsFirst = 5;
            constexpr uint16 kLegsLast  = 9;
            const bool       nation     = log <= static_cast<uint8>(MissionLog::Windurst);
            return nation && id >= kLegsFirst && id <= kLegsLast ? kLegsFirst : id;
        }

        // Where her log stands against the mission the player is on in that
        // log (the goal): done (his leg's completion flag, a nation log's,
        // or the whole mission's; Promathia's "every id below current"), on it (any leg of it),
        // free to take it (the one before its start done, or the log's
        // first), or behind. A standing body's log is hers in memory; a
        // faded one's is her row's
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
            // done his leg, or the whole mission by the other legs (its start
            // is flagged only once the last leg is over)
            const uint16 start = missionStartOf(goal.log, wanted);
            if (done(wanted) || (start != wanted && done(start)))
            {
                return MissionFit::Done;
            }
            if (hers.current != none && missionStartOf(goal.log, hers.current) == start)
            {
                return MissionFit::On;
            }
            return start == 0 || done(start - 1) ? MissionFit::Free : MissionFit::Behind;
        }

        // What stops her whatever her mood: a party, a fight, a camp, the
        // level band, a mission she has not reached. nullopt when nothing does
        // What keeps her from coming whoever asks: another party, a fight, a camp
        auto busyNo(const Facts& f, const CCharEntity* PPawn) -> std::optional<std::string>
        {
            if (PPawn != nullptr)
            {
                if (PPawn->PParty != nullptr)
                {
                    return "I'm with a party already.";
                }
                if (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged())
                {
                    return "I'm in the middle of something!";
                }
            }
            else if (pawn::world::campLeaderOf(f.charid) != 0)
            {
                return "I'm out camping with friends.";
            }
            return std::nullopt;
        }

        auto hardNo(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn, const Goal& goal, const MissionFit fit) -> std::optional<Answer>
        {
            if (const auto busy = busyNo(f, PPawn); busy.has_value())
            {
                return Answer{ false, fit, *busy };
            }

            // a teleport asks for her spell, not her level beside his
            const int side = goal.kind == Goal::Kind::Teleport ? 0 : rules::bandSide(PPlayer->GetMLevel(), levelOf(PPawn, f.level), settings::get<uint8>("pawn.FINDER_BAND"));
            if (side < 0)
            {
                return Answer{ false, fit, pick(f.seed, { "I'd only slow you down.", "I'm not ready for what you're doing." }) };
            }
            if (side > 0)
            {
                return Answer{ false, fit, pick(f.seed, { "Come back when you've grown a little.", "You'd be a liability out there." }) };
            }
            if (fit == MissionFit::Behind)
            {
                return Answer{ false, fit, pick(f.seed, { "I'd like to help, but I'm not on that mission yet!", "I haven't gotten that far. Sorry!" }) };
            }
            return std::nullopt;
        }

        // Her disposition (rules::willing): who you are to the world -- fame
        // in her nation, rank against hers -- against her seeded threshold,
        // `mood` a shout's roll on it and `wear` the towns' wear on top
        // (rules::Wear); a quest asks more than exp, and a mission she is on
        // needs no asking. Having partied with you and her affinity colour
        // the words of a yes, never the yes
        auto softAnswer(const CCharEntity* PPlayer, const Facts& f, const Goal& goal, const Fame& fame, const int mood, const MissionFit fit, const int wear)
            -> Answer
        {
            const uint8 level    = fame.byNation[f.nation];
            const int   rankDiff = static_cast<int>(PPlayer->profile.rank[f.nation]) - static_cast<int>(f.rank[f.nation]);
            if (!rules::willing(goal, fit, level, rankDiff, mood + wear, f.seed))
            {
                if (level <= 1)
                {
                    return { false, fit, pick(f.seed, { "Do I know you?", "Maybe when I've heard of you." }) };
                }
                if (goal.kind == Goal::Kind::Quest)
                {
                    return { false, fit, pick(f.seed, { "Not that one, sorry.", "I've got errands of my own today." }) };
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
            // A yes the towns' wear tipped from a no, checked after every
            // warmer line, so a friend never says it
            if (rules::wornYes(goal, fit, level, rankDiff, mood, wear, f.seed))
            {
                return { true, fit, pick(f.seed, { "Fine, I'll do it already!", "Alright, alright, you win. I'm in.", "You're persistent... fine, let's go.",
                                                   "Okay, okay! Send the invite.", "Ugh, fine. Just this once." }) };
            }
            return { true, fit, pick(f.seed, { "Sure, why not.", "I could use the company.", "Lead the way." }) };
        }

        auto judge(const CCharEntity* PPlayer, const Facts& f, const CCharEntity* PPawn, const Goal& goal, const Fame& fame, const int mood = 0, const int wear = 0)
            -> Answer
        {
            const auto fit = goal.kind == Goal::Kind::Mission ? missionFitOf(PPlayer, f, PPawn, goal) : MissionFit::Free;
            if (auto hard = hardNo(PPlayer, f, PPawn, goal, fit); hard.has_value())
            {
                hard->hard = true;
                return *hard;
            }
            return softAnswer(PPlayer, f, goal, fame, mood, fit, wear);
        }

        // Is the goal one the player's own log holds: CL_S_OK, or why not
        auto goalHeld(const CCharEntity* PPlayer, const Goal& goal) -> uint16
        {
            switch (goal.kind)
            {
                case Goal::Kind::Mission:
                    if ((goal.log > 4 && goal.log != 6) || PPlayer->m_missionLog[goal.log].current == noMission(goal.log))
                    {
                        return CL_S_NO_MISSION;
                    }
                    return CL_S_OK;
                case Goal::Kind::Quest:
                    return goal.log > 10 ? CL_S_MALFORMED : CL_S_OK;
                case Goal::Kind::Teleport:
                    return pawn::teleport::shoutRefusal(PPlayer, goal.log);
                default:
                    return CL_S_OK;
            }
        }

        // A yes the finder took from the shout: the solicit that follows is
        // hers to accept without a second ask, and her contract starts on
        // that yes (pawn charid -> the inviter and the goal)
        struct Consent
        {
            uint32            playerCharID = 0;
            Goal              goal;
            timer::time_point at;            // a consent not answered within kConsentLifetime lapses
            bool              shout = false; // her yes to his shout, not her open contract
            uint32            price = 0;     // a teleport's: the price she named
        };
        std::unordered_map<uint32, Consent> consented;
        constexpr auto                      kConsentLifetime = std::chrono::minutes(2);

        // A teleport invite of his waits on her answer
        auto teleportPending(const uint32 playerCharID) -> bool
        {
            const auto now = timer::now();
            return std::ranges::any_of(consented, [&](const auto& entry)
                                       {
                                           const auto& c = entry.second;
                                           return c.playerCharID == playerCharID && c.goal.kind == Goal::Kind::Teleport && now - c.at <= kConsentLifetime;
                                       });
        }

        // Joined just now on a yes to his shout, until gatherOrHold asks
        // (joinedFromShout)
        std::unordered_set<uint32> joinedShout;

        // Released by her player this moment (release): the party she leaves
        // on the way holds her for nobody (noteLeft)
        std::unordered_set<uint32> releasing;

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
        // When each body last heard a player's shout, by player: the shout's
        // rotation (rules::pickHeard) hears those longest unheard first, the
        // friend seat and the affinity seat included. His next shout drops
        // an entry older than kHeardKept, which to the rotation is the same
        // as never
        std::unordered_map<uint32, std::unordered_map<uint32, timer::time_point>> lastHeard;
        constexpr auto kHeardKept = std::chrono::hours(1);

        // How worn down the towns are by a player's shouts, by wearKey: one
        // pressure for each player and goal, on every body asked for it
        // (rules::Wear). Kept in memory: a server start begins the towns
        // fresh
        std::unordered_map<uint64, rules::Wear> townWear;

        // The player and the goal, at the finder's own grain: its kind and log
        auto wearKey(const uint32 playerCharID, const Goal& goal) -> uint64
        {
            return (static_cast<uint64>(playerCharID) << 16) | (static_cast<uint64>(goal.kind) << 8) | goal.log;
        }

        auto wearSeconds() -> double
        {
            return std::chrono::duration<double>(timer::now().time_since_epoch()).count();
        }

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
        auto shoutedYes(const CCharEntity* PPlayer, const uint32 charid, const Goal& goal) -> const Responder*
        {
            const auto* held = currentShout(PPlayer->id);
            if (held == nullptr || !(held->shout.goal == goal))
            {
                return nullptr;
            }
            const auto it = std::ranges::find_if(held->shout.rows, [&](const Responder& r)
            {
                return r.c.answer.yes && r.c.charid == charid;
            });
            return it != held->shout.rows.end() ? &*it : nullptr;
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
        else if (kind == "teleport")
        {
            goal.kind = Goal::Kind::Teleport;
        }
        goal.log = static_cast<uint8>(std::clamp(log, 0, 255));
        return goal;
    }

    auto kindName(const Goal& goal) -> const char*
    {
        switch (goal.kind)
        {
            case Goal::Kind::Mission:
                return "mission";
            case Goal::Kind::Quest:
                return "quest";
            case Goal::Kind::Teleport:
                return "teleport"; // eight letters, the width of cardian_party_memory.contract
            default:
                return "exp";
        }
    }

    auto shout(const CCharEntity* PPlayer, const Goal& goal, const bool again, uint16& refusal, uint32& waitMs) -> const Shout*
    {
        refusal = CL_S_OK;
        waitMs  = 0;
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            refusal = CL_S_REFUSED;
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
            refusal = CL_S_PARTY_FULL;
            return nullptr;
        }
        if (pawn::teleport::hiringFor(PPlayer->id) || teleportPending(PPlayer->id))
        {
            refusal = CL_S_TELEPORT_HIRED; // one contract at a time while a teleport is under way
            return nullptr;
        }
        if (refusal = goalHeld(PPlayer, goal); refusal != CL_S_OK)
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
            refusal = CL_S_COOLING_DOWN;
            waitMs  = wait;
            return nullptr;
        }

        // Everyone online in a city or a town, wherever he shouts from (the
        // user, 2026-10-02): a body in the field or a dungeon is busy and does
        // not hear it. Each with a mood for this shout, and her distance in
        // zone lines from him
        std::vector<Candidate> pool;
        const auto             fame   = fameOf(PPlayer);
        const auto             hopsTo = pawn::travel::hops({ PPlayer->getZone() });

        // The towns' wear for this goal: his shouts before, fading
        // (rules::Wear), the same bonus on every mood; forgotten once faded
        // to nothing
        const auto   wearAt   = wearKey(PPlayer->id, goal);
        const double wearNow  = wearSeconds();
        const double halfLife = settings::get<double>("pawn.FINDER_WEAR_HALF_LIFE");
        double       pressure = 0.0;
        if (const auto it = townWear.find(wearAt); it != townWear.end())
        {
            pressure = rules::wornNow(it->second, wearNow, halfLife);
            if (pressure < rules::kWearForgotten)
            {
                townWear.erase(it);
                pressure = 0.0;
            }
        }
        const int wear = rules::wearBonus(pressure, settings::get<int>("pawn.FINDER_WEAR_STEP"));
        // a teleport is heard by those who cast it now, and nobody else
        const auto teleporters = goal.kind == Goal::Kind::Teleport ? std::optional<pawn::teleport::Responders>(std::in_place, goal.log) : std::nullopt;
        for (const auto& f : allFacts(PPlayer->id))
        {
            const auto* PPawn = pawn::findPawn(f.charid);
            auto*       PHere = zoneOf(PPawn, f.posZone);
            const bool inTown = teleporters.has_value() ? pawn::teleport::isTown(PHere) : cityOrTown(PHere);
            if (!rules::hears(inTown, f.nation, goal) || !available(PPlayer, f, PPawn) || (PPawn == nullptr && !pawn::seats::has(f.charid)))
            {
                continue;
            }
            if (teleporters.has_value() && !teleporters->answers(f.charid, PPawn, f.job, f.level, f.sjob, f.slvl))
            {
                continue;
            }
            // A town's own White Mage (world.h keepResidents) answers its teleport shouts alone
            if (!teleporters.has_value() && pawn::world::isTeleporter(f.charid))
            {
                continue;
            }
            Candidate c;
            c.name     = f.name;
            c.job      = PPawn != nullptr ? static_cast<uint8>(PPawn->GetMJob()) : f.job;
            c.level    = levelOf(PPawn, f.level);
            c.sjob     = PPawn != nullptr ? static_cast<uint8>(PPawn->GetSJob()) : f.sjob;
            c.slvl     = PPawn != nullptr ? PPawn->GetSLevel() : f.slvl;
            c.affinity = f.affinity;
            c.race     = f.race;
            c.nation   = f.nation;
            c.rank     = f.rank[f.nation];
            c.zone     = PHere->getName();
            c.zoneId   = static_cast<uint16>(PHere->GetID());
            const auto hop = hopsTo.find(c.zoneId);
            c.hops     = hop != hopsTo.end() ? hop->second : std::numeric_limits<uint32>::max();
            c.presence = presenceOf(PPlayer, PPawn, f.charid, PHere);
            if (teleporters.has_value())
            {
                // a White Mage free to come names her price; one who is not stays quiet
                if (busyNo(f, PPawn).has_value())
                {
                    continue;
                }
                c.price  = pawn::teleport::rules::priceFor(f.charid, goal.log);
                c.answer = Answer{ true, MissionFit::Free, pawn::teleport::answerLine(pawn::teleport::rules::rollOf(f.charid, goal.log) >> 8, goal.log) };
            }
            else
            {
                c.answer = judge(PPlayer, f, PPawn, goal, fame, xirand::GetRandomNumber(rules::kMoodMin, rules::kMoodMax + 1), wear);
            }
            c.charid   = f.charid;
            c.friendly = f.partied || f.affinity > 0;
            pool.push_back(std::move(c));
        }

        // A teleport is answered by the White Mages of his own hub alone (the user, 2026-10-21): he
        // shouts for one in town (teleport::shoutRefusal), where a teleporter always is (world.h
        // keepResidents)
        if (teleporters.has_value())
        {
            const auto hub = pawn::teleport::hubOf(PPlayer->loc.zone);
            std::erase_if(pool, [&](const Candidate& c) { return pawn::teleport::hubOf(zoneutils::GetZone(static_cast<xi::ZoneId>(c.zoneId))) != hub; });
        }

        // Who of them hear it, and in what order (rules::pickHeard): those who
        // have heard him least recently first; a friend seat and an affinity
        // seat, enough yeses to fill the party when the crowd has them (the
        // user, 2026-09-13: one shout should be able to form a party; a
        // re-shout is for the picky), up to three who cannot come -- out of
        // the band, or short of the mission -- and the rest a mix of those
        // who could; nearest first
        auto& heard = lastHeard[PPlayer->id];
        std::erase_if(heard, [now = timer::now()](const auto& entry) { return now - entry.second > kHeardKept; });
        const int                band = settings::get<uint8>("pawn.FINDER_BAND");
        std::vector<rules::Seat> seats;
        seats.reserve(pool.size());
        for (std::size_t i = 0; i < pool.size(); ++i)
        {
            const auto& c      = pool[i];
            const auto  it     = heard.find(c.charid);
            const bool  cannot = !c.answer.yes && goal.kind != Goal::Kind::Teleport && (c.answer.fit == MissionFit::Behind || rules::bandSide(PPlayer->GetMLevel(), c.level, band) != 0);
            seats.push_back(rules::Seat{ .index    = i,
                                         .yes      = c.answer.yes,
                                         .cannot   = cannot,
                                         .friendly = c.friendly,
                                         .affinity = c.affinity,
                                         .hops     = c.hops,
                                         .heardAt  = it != heard.end() ? it->second.time_since_epoch().count() : std::numeric_limits<int64>::min() });
        }
        const auto        count        = static_cast<std::size_t>(xirand::GetRandomNumber(static_cast<int>(rules::kHeardMin), static_cast<int>(rules::kHeardMax) + 1));
        const std::size_t need         = goal.kind == Goal::Kind::Teleport ? 1 : 6 - std::min<std::size_t>(6, partySize(PPlayer)); // one White Mage takes a party
        const auto        picked       = goal.kind == Goal::Kind::Teleport ? rules::pickNearest(std::move(seats), pawn::teleport::rules::kAnswersAtMost, xirand::rng())
                                                                           : rules::pickHeard(std::move(seats), count, need, xirand::rng());
        const auto        nameAt       = [&](const std::optional<std::size_t>& index) { return index.has_value() ? pool[*index].name : std::string("empty"); };
        const std::string friendSeat   = nameAt(picked.friendSeat);
        const std::string affinitySeat = nameAt(picked.affinitySeat);

        // Her timing: the first voice after a beat, then uneven gaps -- most
        // short, now and then a lull -- and each deliberates a while. Paced
        // so a crowd of twenty is all heard in about twenty seconds
        Held made;
        made.madeAt     = timer::now();
        made.shout.id   = ++lastShoutId;
        made.shout.goal = goal;
        uint32 at       = xirand::GetRandomNumber(700, 2200);

        std::vector<rules::Reply> replies;
        replies.reserve(picked.seats.size());
        for (const auto& s : picked.seats)
        {
            auto& c         = pool[s.index];
            heard[c.charid] = made.madeAt;
            replies.push_back(rules::Reply{ .yes = c.answer.yes, .hard = c.answer.hard });
            Responder r;
            r.c        = std::move(c);
            r.revealMs = at;
            r.decideMs = xirand::GetRandomNumber(1500, 5200);
            const bool lull = xirand::GetRandomNumber(0, 6) == 0;
            at += lull ? xirand::GetRandomNumber(1500, 3000) : xirand::GetRandomNumber(250, 1200);
            made.shout.rows.push_back(std::move(r));
        }
        // A soft no among those who heard it wears the towns down for his
        // next shout for this goal (rules::Wear)
        {
            const auto before = townWear.find(wearAt);
            const auto after  = rules::wornAfterShout(before != townWear.end() ? before->second : rules::Wear{}, replies, wearNow, halfLife);
            if (after.pressure > 0.0)
            {
                townWear[wearAt] = after;
            }
        }
        const auto yes      = std::ranges::count_if(made.shout.rows, [](const Responder& r) { return r.c.answer.yes; });
        const auto known    = std::ranges::count_if(made.shout.rows, [](const Responder& r) { return r.c.friendly; });
        const auto inBand   = std::ranges::count_if(pool, [&](const Candidate& c) { return rules::bandSide(PPlayer->GetMLevel(), c.level, band) == 0; });
        const auto hopsText = [](const uint32 hops) { return hops == std::numeric_limits<uint32>::max() ? std::string("no route") : std::to_string(hops); };
        ShowInfoFmt("pawn: {} shouts ({}, log {}): {} in reach, {} of them in the band, {} hear it ({} strangers), {} would come, {} needed to fill the party, "
                    "friend seat: {}, affinity seat: {}, zone lines away: nearest {}, farthest {}, the towns' wear +{}",
                    PPlayer->getName(), kindName(goal), goal.log, pool.size(), inBand, made.shout.rows.size(), made.shout.rows.size() - static_cast<std::size_t>(known),
                    yes, need, friendSeat, affinitySeat,
                    made.shout.rows.empty() ? "-" : hopsText(made.shout.rows.front().c.hops),
                    made.shout.rows.empty() ? "-" : hopsText(made.shout.rows.back().c.hops), wear);
        if (!made.shout.rows.empty())
        {
            shoutedAt[PPlayer->id] = made.madeAt;
        }
        made.shout.waitMs = shoutWaitMs(PPlayer->id);
        auto [it, inserted] = shouts.insert_or_assign(PPlayer->id, std::move(made));
        return &it->second.shout;
    }

    auto heldShout(const uint32 playerCharID) -> uint32
    {
        const auto* held = currentShout(playerCharID);
        return held != nullptr ? held->shout.id : 0;
    }

    void endShout(const uint32 playerCharID)
    {
        if (shouts.erase(playerCharID) > 0)
        {
            ShowInfoFmt("pawn: {}'s shout has done its work and ends", pawn::seats::nameOf(playerCharID));
        }
    }

    void snapshot(CCharEntity* PPawn)
    {
        if (PPawn == nullptr)
        {
            return;
        }
        const auto numbers = pawn::statusNumbers(PPawn);
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
                         numbers.total[0], numbers.total[1], numbers.total[2], numbers.total[3],
                         numbers.total[4], numbers.total[5], numbers.total[6],
                         numbers.bonus[0], numbers.bonus[1], numbers.bonus[2], numbers.bonus[3],
                         numbers.bonus[4], numbers.bonus[5], numbers.bonus[6],
                         numbers.attack, numbers.defence);
    }

    namespace
    {
        // What her body says: her jobs, her rank in her nation, what she
        // wears, her numbers as she stands
        void fromBody(Peek& p, CCharEntity* PPawn)
        {
            p.standing = true;
            p.job      = static_cast<uint8>(PPawn->GetMJob());
            p.level    = PPawn->GetMLevel();
            p.sjob     = static_cast<uint8>(PPawn->GetSJob());
            p.slvl     = PPawn->GetSLevel();
            p.rank     = p.nation < 3 ? PPawn->profile.rank[p.nation] : p.rank;
            for (uint8 equipSlot = SLOT_MAIN; equipSlot <= SLOT_BACK; ++equipSlot)
            {
                if (const auto* PItem = PPawn->getEquip(static_cast<SLOTTYPE>(equipSlot)); PItem != nullptr)
                {
                    p.items[equipSlot] = PItem->getID();
                }
            }
            p.known   = true;
            p.hp      = static_cast<uint32>(std::max(0, PPawn->health.hp));
            p.maxhp   = static_cast<uint32>(std::max(0, PPawn->GetMaxHP()));
            p.mp      = static_cast<uint32>(std::max(0, PPawn->health.mp));
            p.maxmp   = static_cast<uint32>(std::max(0, PPawn->GetMaxMP()));
            p.numbers = pawn::statusNumbers(PPawn);
        }

        // One of his own -- his alt, a cardian his account owns -- has no
        // census row: her nation, ranks and jobs off her character's rows,
        // the rest off her body when she stands, else what she wears off
        // her rows
        auto peekOwn(const uint32 charid) -> std::optional<Peek>
        {
            const auto rset = db::preparedStmt("SELECT c.nation, s.mjob, s.mlvl, s.sjob, s.slvl, p.rank_sandoria, p.rank_bastok, p.rank_windurst "
                                               "FROM chars c JOIN char_stats s ON s.charid = c.charid JOIN char_profile p ON p.charid = c.charid WHERE c.charid = ?",
                                               charid);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            Peek                       p;
            const std::array<uint8, 3> ranks{ rset->get<uint8>("rank_sandoria"), rset->get<uint8>("rank_bastok"), rset->get<uint8>("rank_windurst") };
            p.nation = rset->get<uint8>("nation");
            p.rank   = p.nation < ranks.size() ? ranks[p.nation] : 1;
            p.job    = rset->get<uint8>("mjob");
            p.level  = rset->get<uint8>("mlvl");
            p.sjob   = rset->get<uint8>("sjob");
            p.slvl   = rset->get<uint8>("slvl");
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                fromBody(p, PPawn);
                return p;
            }
            const auto gear = db::preparedStmt("SELECT e.equipslotid, i.itemId FROM char_equip e JOIN char_inventory i "
                                               "ON i.charid = e.charid AND i.location = e.containerid AND i.slot = e.slotid WHERE e.charid = ?",
                                               charid);
            while (gear && gear->next())
            {
                if (const auto slot = gear->get<uint8>("equipslotid"); slot < p.items.size())
                {
                    p.items[slot] = gear->get<uint16>("itemId");
                }
            }
            return p;
        }
    } // namespace

    auto peek(const CCharEntity* PPlayer, const uint32 charid) -> std::optional<Peek>
    {
        if (PPlayer == nullptr || charid == 0)
        {
            return std::nullopt;
        }
        // Someone in his current shout, held by his own open contract, a
        // recruit of his linkshell, or a member of it (the Linkshell page's
        // Examine); one of his own has no census row
        const bool member = pawn::club::memberOf(PPlayer, charid).has_value();
        const auto facts  = factsOf(PPlayer->id, charid);
        if (!facts.has_value())
        {
            return member ? peekOwn(charid) : std::nullopt;
        }
        const auto* held       = currentShout(PPlayer->id);
        const bool  shouted    = held != nullptr && std::ranges::any_of(held->shout.rows, [&](const Responder& r) { return r.c.charid == charid; });
        const auto  contracted = openContractOf(charid);
        if (!member && !shouted && !(contracted.has_value() && contracted->playerCharID == PPlayer->id) && !pawn::club::isRecruit(PPlayer, charid))
        {
            return std::nullopt;
        }
        Peek p;
        p.nation   = facts->nation;
        p.rank     = facts->rank[facts->nation];
        p.affinity = facts->affinity;
        p.missions = facts->together;
        if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
        {
            fromBody(p, PPawn);
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
            p.known = true;
            p.hp    = rset->get<uint32>("hp");
            p.maxhp = rset->get<uint32>("maxhp");
            p.mp    = rset->get<uint32>("mp");
            p.maxmp = rset->get<uint32>("maxmp");
            std::size_t i = 0;
            for (const auto* stat : { "str", "dex", "vit", "agi", "int", "mnd", "chr" })
            {
                p.numbers.total[i] = rset->get<int16>(fmt::format("{}_t", stat));
                p.numbers.bonus[i] = rset->get<int16>(fmt::format("{}_b", stat));
                ++i;
            }
            p.numbers.attack  = rset->get<uint16>("att");
            p.numbers.defence = rset->get<uint16>("def");
        }
        const auto rset = db::preparedStmt("SELECT slot, itemid FROM cardian_wardrobe WHERE name = ? ORDER BY slot", facts->name);
        while (rset && rset->next())
        {
            if (const auto slot = rset->get<uint8>("slot"); slot < p.items.size())
            {
                p.items[slot] = rset->get<uint16>("itemid");
            }
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

    auto shoutedNames() -> std::unordered_set<std::string>
    {
        std::unordered_set<std::string> names;
        const auto                      now = timer::now();
        for (auto it = shouts.begin(); it != shouts.end();)
        {
            if (now - it->second.madeAt > kShoutLifetime || zoneutils::GetChar(it->first) == nullptr)
            {
                it = shouts.erase(it);
                continue;
            }
            for (const auto& r : it->second.shout.rows)
            {
                if (r.c.answer.yes)
                {
                    names.insert(r.c.name);
                }
            }
            ++it;
        }
        return names;
    }

    auto invite(CCharEntity* PPlayer, const uint32 charid, const Goal& goal, std::string& line) -> uint16
    {
        line.clear();
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return CL_S_REFUSED;
        }
        if (PPlayer->PParty != nullptr && PPlayer->PParty->GetLeader() != PPlayer)
        {
            return CL_S_NOT_LEADER;
        }
        if (partyFull(PPlayer))
        {
            return CL_S_PARTY_FULL;
        }
        // One contract at a time while a teleport is under way, its invite
        // still unanswered included: one White Mage at a time. Every Link
        // request runs on the map thread one after another, so the second
        // invite always finds the first's consent
        if (pawn::teleport::hiringFor(PPlayer->id) || teleportPending(PPlayer->id))
        {
            return CL_S_TELEPORT_HIRED;
        }
        // The list's own gate: an unrecruited census body, a yes in the
        // player's current shout for this goal, still in a city or a town
        // (where the shout reached her), then what stops her whatever she
        // said before. Her zone is her body's when she stands and her row's
        // when she is faded
        const auto facts = charid != 0 ? factsOf(PPlayer->id, charid) : std::nullopt;
        if (!facts.has_value())
        {
            return CL_S_NOT_ADVENTURER;
        }
        auto* PPawn = pawn::findPawn(charid);
        // Her open contract with him is her yes, to its own goal: no shout,
        // and nothing asked again of what she agreed to. From another zone
        // the club's rule holds her until gathered (gatherOrHold)
        const auto held  = openContractOf(charid);
        const auto asked = held.has_value() ? held->goal : goal;
        if (held.has_value() && held->playerCharID != PPlayer->id)
        {
            return CL_S_CONTRACTED;
        }
        if (!held.has_value())
        {
            if (currentShout(PPlayer->id) == nullptr)
            {
                return CL_S_SHOUT_FADED;
            }
            if (shoutedYes(PPlayer, charid, goal) == nullptr)
            {
                return CL_S_NOT_A_YES;
            }
            if (PPawn == nullptr && !pawn::seats::has(charid))
            {
                return CL_S_AWAY;
            }
            if (auto* PHere = zoneOf(PPawn, facts->posZone); !(goal.kind == Goal::Kind::Teleport ? pawn::teleport::isTown(PHere) : cityOrTown(PHere)))
            {
                return CL_S_LEFT_TOWN;
            }
            if (goal.kind == Goal::Kind::Teleport)
            {
                if (const auto refusal = pawn::teleport::shoutRefusal(PPlayer, goal.log); refusal != CL_S_OK)
                {
                    return refusal;
                }
            }
            const auto fit = goal.kind == Goal::Kind::Mission ? missionFitOf(PPlayer, *facts, PPawn, goal) : MissionFit::Free;
            if (const auto hard = hardNo(PPlayer, *facts, PPawn, goal, fit); hard.has_value())
            {
                line = hard->line;
                return CL_S_DECLINES;
            }
        }
        if (PPawn == nullptr)
        {
            if (const auto status = standFaded(PPlayer, charid); status != CL_S_OK)
            {
                return status;
            }
            PPawn = pawn::findPawn(charid);
        }
        if (PPawn == nullptr)
        {
            return CL_S_CANNOT_STAND;
        }

        // Knocked out, she is invited all the same, as the game lets a
        // knocked-out player be: in his party, the party's Home point reaches her
        if (PPawn->PParty != nullptr)
        {
            return CL_S_IN_A_PARTY;
        }
        if (PPawn->InvitePending.entity.UniqueNo != 0)
        {
            return CL_S_INVITE_PENDING;
        }

        const auto* quoted = held.has_value() ? nullptr : shoutedYes(PPlayer, charid, goal);
        consented[charid]  = Consent{ PPlayer->id, asked, timer::now(), !held.has_value(), quoted != nullptr ? quoted->c.price : 0 };
        PPawn->InvitePending.entity.UniqueNo = PPlayer->id;
        PPawn->InvitePending.entity.ActIndex = PPlayer->targid;
        PPawn->InvitePending.kind            = PartyKind::Party;
        PPawn->pushPacket<GP_SERV_COMMAND_GROUP_SOLICIT_REQ>(PPawn->id, PPawn->targid, PPlayer->getName(), PartyKind::Party);
        ShowInfoFmt("pawn: {} invites {} from the party finder, for {}{}", PPlayer->getName(), PPawn->getName(), kindName(asked), held.has_value() ? ", her open contract" : "");
        return CL_S_OK;
    }

    auto accepts(CCharEntity* PPawn) -> bool
    {
        if (PPawn == nullptr)
        {
            return false;
        }
        const auto* PInviter = zoneutils::GetCharFromWorld(PPawn->InvitePending.entity.UniqueNo, PPawn->InvitePending.entity.ActIndex);
        // Held for her player while her contract is open: his invite alone
        // (by id: he may be mid-zone as she answers)
        const auto held = openContractOf(PPawn->id);
        if (held.has_value() && PPawn->InvitePending.entity.UniqueNo != held->playerCharID)
        {
            ShowInfoFmt("pawn: {} declines {}'s invite: she is under contract with another player", PPawn->getName(), PInviter != nullptr ? PInviter->getName() : "someone");
            consented.erase(PPawn->id);
            return false;
        }
        const auto it   = consented.find(PPawn->id);
        const bool wild = pawn::seats::isWorlds(PPawn->id);
        if (it == consented.end())
        {
            if (!wild || held.has_value())
            {
                return true; // hers to accept: an alt, a recruit, or her contract's player
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
        if (consent.shout)
        {
            joinedShout.insert(PPawn->id);
            // his shout for the goal found someone: the towns' wear for it starts over
            townWear.erase(wearKey(consent.playerCharID, consent.goal));
        }
        db::preparedStmt("UPDATE cardian_party_memory SET contract = '' WHERE pawn_charid = ? AND player_charid <> ? AND contract <> ''", PPawn->id, consent.playerCharID);
        db::preparedStmt("INSERT INTO cardian_party_memory (player_charid, pawn_charid, last_partied, contract) VALUES (?, ?, NOW(), ?) "
                         "ON DUPLICATE KEY UPDATE contract = VALUES(contract), last_partied = NOW()",
                         consent.playerCharID, PPawn->id, kindName(consent.goal));
        ShowInfoFmt("pawn: {} joins {} under the {} contract", PPawn->getName(), PInviter->getName(), kindName(consent.goal));
        if (consent.goal.kind == Goal::Kind::Teleport)
        {
            pawn::teleport::hired(consent.playerCharID, PPawn->id, consent.goal.log, consent.shout, consent.price);
        }
        return true;
    }

    auto joinedFromShout(const uint32 charid) -> bool
    {
        return joinedShout.erase(charid) > 0;
    }

    auto contractWith(const uint32 charid, const uint32 playerCharID) -> const char*
    {
        const auto* c = contractFor(charid, playerCharID);
        return c != nullptr ? kindName(c->goal) : "";
    }

    // Her party's day-to-day: what she was to the party she was in
    void forgetParty(const uint32 charid)
    {
        contracts.erase(charid);
        noContract.erase(charid);
        expBank.erase(charid);
        joinedShout.erase(charid);
    }

    void noteLeft(const uint32 charid)
    {
        forgetParty(charid);
        // Out of his party her contract holds (the user, 2026-10-25): she is
        // his hire until he releases her, kept for him where she stands --
        // in a town at his lounge with his members (club.cpp) -- and invited
        // again with no shout. A teleport hire's ends: hers was one service
        if (const auto held = openContractOf(charid); held.has_value() && !held->pearl && held->goal.kind != Goal::Kind::Teleport &&
                                                      !releasing.contains(charid) && pawn::world::keepFor(charid, held->playerCharID))
        {
            ShowInfoFmt("pawn: {} is out of {}'s party; her {} contract holds her for him", held->name, pawn::seats::nameOf(held->playerCharID), kindName(held->goal));
            return;
        }
        endContract(charid);
    }

    void endContract(const uint32 charid)
    {
        forgetParty(charid);
        if (const auto held = openContractOf(charid); held.has_value() && !held->pearl)
        {
            db::preparedStmt("UPDATE cardian_party_memory SET contract = '' WHERE pawn_charid = ? AND contract <> ''", charid);
            ShowInfoFmt("pawn: {}'s {} contract with {} ends", held->name, kindName(held->goal), pawn::seats::nameOf(held->playerCharID));
        }
        // His linkshell's pearl keeps her for him where she stands; anyone
        // else the world's clocks run on again
        if (const auto pearl = pawn::club::pearlOf(charid); !pearl.has_value() || !pawn::world::keepFor(charid, pearl->playerCharID))
        {
            pawn::world::endHold(charid);
        }
    }

    auto openContractOf(const uint32 charid) -> std::optional<OpenContract>
    {
        const auto rset = db::preparedStmt("SELECT m.player_charid, m.contract, c.charname FROM cardian_party_memory m JOIN chars c ON c.charid = m.pawn_charid "
                                           "JOIN cardian_census x ON x.charid = m.pawn_charid "
                                           "WHERE m.pawn_charid = ? AND m.contract <> '' AND x.recruited = 0 ORDER BY m.last_partied DESC LIMIT 1",
                                           charid);
        if (rset && rset->next())
        {
            return OpenContract{ .charid       = charid,
                                 .playerCharID = rset->get<uint32>("player_charid"),
                                 .name         = rset->get<std::string>("charname"),
                                 .goal         = goalFrom(rset->get<std::string>("contract"), 0) };
        }
        // His pearl on one of the world's holds her between contracts
        const auto pearl = pawn::club::pearlOf(charid);
        if (!pearl.has_value())
        {
            return std::nullopt;
        }
        const auto held = db::preparedStmt("SELECT c.charname FROM chars c JOIN cardian_census x ON x.charid = c.charid WHERE c.charid = ? AND x.recruited = 0", charid);
        if (!held || !held->next())
        {
            return std::nullopt;
        }
        return OpenContract{ .charid = charid, .playerCharID = pearl->playerCharID, .name = held->get<std::string>("charname"), .goal = Goal{}, .pearl = true };
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
        // and the world's wearing a pearl of his shell with no contract of theirs
        for (const auto charid : pawn::club::wearersOf(playerCharID))
        {
            if (std::ranges::any_of(out, [&](const OpenContract& c) { return c.charid == charid; }))
            {
                continue;
            }
            if (const auto held = openContractOf(charid); held.has_value() && held->pearl)
            {
                out.push_back(*held);
            }
        }
        std::ranges::sort(out, {}, &OpenContract::name);
        return out;
    }

    auto contractsOf(const CCharEntity* PPlayer) -> std::vector<ContractView>
    {
        std::vector<ContractView> out;
        if (PPlayer == nullptr)
        {
            return out;
        }
        for (const auto& c : openContracts(PPlayer->id))
        {
            if (c.pearl)
            {
                continue; // held by his pearl alone: the Linkshell page's, not a contract
            }
            ContractView view{ .charid = c.charid, .name = c.name, .goal = c.goal };
            if (const auto* PPawn = pawn::findPawn(c.charid); PPawn != nullptr)
            {
                view.job   = static_cast<uint8>(PPawn->GetMJob());
                view.level = PPawn->GetMLevel();
                view.zone  = static_cast<uint16>(PPawn->getZone());
                view.state = PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty ? ContractState::Party : ContractState::Standing;
            }
            else if (const auto rset = db::preparedStmt("SELECT s.mjob, s.mlvl, c.pos_zone FROM chars c JOIN char_stats s ON s.charid = c.charid WHERE c.charid = ?", c.charid);
                     rset && rset->next())
            {
                view.job   = rset->get<uint8>("mjob");
                view.level = rset->get<uint8>("mlvl");
                view.zone  = rset->get<uint16>("pos_zone");
                view.state = pawn::seats::has(c.charid) ? ContractState::Faded : ContractState::Out;
            }
            out.push_back(std::move(view));
        }
        return out;
    }

    auto release(CCharEntity* PPlayer, const uint32 charid) -> uint16
    {
        if (PPlayer == nullptr)
        {
            return CL_S_REFUSED;
        }
        const auto held = charid != 0 ? openContractOf(charid) : std::nullopt;
        if (!held.has_value() || held->playerCharID != PPlayer->id)
        {
            return CL_S_NO_CONTRACT;
        }
        // In his party, Release takes her out of it as well: one step, no
        // kick to remember (the user, 2026-09-24). Leaving keeps a contract
        // (noteLeft); Release is what ends it
        if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr && PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty)
        {
            ShowInfoFmt("pawn: {} releases {} from the party and her contract", PPlayer->getName(), PPawn->getName());
            releasing.insert(charid);
            PPawn->PParty->RemoveMember(PPawn);
            releasing.erase(charid);
        }
        endContract(charid);
        pawn::forgetGuestGambits(charid);
        return CL_S_OK;
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

    auto standFaded(const CCharEntity* PPlayer, const uint32 charid) -> uint16
    {
        if (PPlayer == nullptr || charid == 0 || pawn::findPawn(charid) != nullptr)
        {
            return CL_S_OK;
        }
        if (!pawn::seats::heldFor(charid, PPlayer->id))
        {
            return CL_S_AWAY;
        }
        if (pawn::world::campLeaderOf(charid) != 0)
        {
            return CL_S_IN_A_PARTY;
        }
        if (!pawn::seats::inviteStand(charid))
        {
            ShowInfoFmt("pawn: {} invites {}; she is faded and cannot stand now (nobody near her zone, the cap full of party members, or her stand waits on a retry)",
                        PPlayer->getName(), pawn::seats::nameOf(charid));
            return CL_S_CANNOT_STAND;
        }
        const auto* PPawn = pawn::findPawn(charid);
        ShowInfoFmt("pawn: {} invites {}; she was faded and stands for it", PPlayer->getName(), PPawn != nullptr ? PPawn->getName() : pawn::seats::nameOf(charid));
        return PPawn != nullptr ? CL_S_OK : CL_S_CANNOT_STAND;
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
        // One of the world's is the Party Finder's to recruit, save one his
        // own open contract holds: she is his to invite by name
        const auto held = openContractOf(charid);
        if (pawn::seats::isWorlds(charid) && !(held.has_value() && held->playerCharID == PPlayer->id))
        {
            const auto name = pawn::seats::nameOf(charid);
            PPlayer->pushPacket<GP_SERV_COMMAND_MESSAGE>(PPlayer, 0, 0, MsgStd::InvitationDeclined);
            PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, fmt::format("{} joins a party only through a Shout.", name));
            ShowInfoFmt("pawn: {} invites {} by name; refused, she is the world's (the Party Finder recruits her)", PPlayer->getName(), name);
            return true;
        }
        // The player's own, or his contract's, faded: stood so the handler finds her
        if (pawn::findPawn(charid) == nullptr)
        {
            if (const auto status = standFaded(PPlayer, charid); status != CL_S_OK && pawn::findPawn(charid) == nullptr)
            {
                ShowInfoFmt("pawn: {} invites {} by name; she does not stand (outcome 0x{:04X}), so the game drops the invite", PPlayer->getName(), pawn::seats::nameOf(charid), status);
            }
        }
        return false;
    }
} // namespace pawn::finder
