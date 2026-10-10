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

#include "club.h"
#include "club_math.h"
#include "club_trade.h"

#include "cardian_link.h"
#include "errands.h"
#include "party_finder.h"
#include "pawn.h"
#include "pawn_items.h"
#include "pawn_travel.h"
#include "players.h"
#include "professions.h"
#include "seats.h"
#include "world.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "enums/chat_message_type.h"
#include "enums/party_kind.h"
#include "item_container.h"
#include "items/item_linkshell.h"
#include "items/transactions/item_claim.h"
#include "linkshell.h"
#include "navmesh/navmesh.h"
#include "packets/c2s/0x033_trade_res.h"
#include "packets/c2s/0x0c4_group_comlink_active.h"
#include "packets/c2s/validation.h"
#include "packets/s2c/0x017_chat_std.h"
#include "packets/s2c/0x05a_motionmes.h"
#include "packets/s2c/0x0dc_group_solicit_req.h"
#include "party.h"
#include "pause/pause.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::club
{
    namespace
    {
        using namespace cardian::link;
        namespace rules = cardian::club;

        // Who wears which shell's pearl, off the items (read): by the wearer
        std::unordered_map<uint32, Pearl> worn;

        // char_jobs' level columns, by job id
        constexpr std::array<std::string_view, 23> kJobColumns{ "",    "war", "mnk", "whm", "blm", "rdm", "thf", "pld", "drk", "bst", "brd", "rng",
                                                                "sam", "nin", "drg", "smn", "blu", "cor", "pup", "dnc", "sch", "geo", "run" };

        auto nameOf(const uint32 charid) -> std::string
        {
            if (const auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                return PPawn->getName();
            }
            return pawn::seats::nameOf(charid);
        }

        // Every Linkshell's holder, then every linkshell item worn in a link
        // slot whose shell somebody holds, broken ones left out: the items as
        // the game last saved them, which it does at every equip and every
        // break
        void read()
        {
            std::unordered_map<uint32, std::pair<uint32, uint32>> holders; // lsid -> holder, his account
            if (const auto rset = db::preparedStmt("SELECT i.charid, c.accid, i.extra FROM char_inventory i JOIN chars c ON c.charid = i.charid WHERE i.itemId = ?",
                                                   rules::kLinkshell))
            {
                while (rset->next())
                {
                    uint8 extra[CItem::extra_size]{};
                    db::extractFromBlob(rset, "extra", extra);
                    if (const auto lsid = rules::lsidOf(extra, sizeof(extra)); lsid != 0 && rules::lsTypeOf(extra, sizeof(extra)) != rules::kLsTypeBroken)
                    {
                        holders[lsid] = { rset->get<uint32>("charid"), rset->get<uint32>("accid") };
                    }
                }
            }
            std::unordered_map<uint32, Pearl> found;
            if (const auto rset = db::preparedStmt("SELECT e.charid, i.extra FROM char_equip e JOIN char_inventory i "
                                                   "ON i.charid = e.charid AND i.location = e.containerid AND i.slot = e.slotid "
                                                   "WHERE e.equipslotid IN (?, ?) AND i.itemId IN (?, ?)",
                                                   rules::kLinkSlot1, rules::kLinkSlot2, rules::kPearlsack, rules::kLinkpearl))
            {
                while (rset->next())
                {
                    uint8 extra[CItem::extra_size]{};
                    db::extractFromBlob(rset, "extra", extra);
                    const auto lsid = rules::lsidOf(extra, sizeof(extra));
                    const auto it   = holders.find(lsid);
                    if (lsid == 0 || it == holders.end() || rules::lsTypeOf(extra, sizeof(extra)) == rules::kLsTypeBroken)
                    {
                        continue;
                    }
                    found.emplace(rset->get<uint32>("charid"), Pearl{ it->second.second, it->second.first, lsid });
                }
            }
            worn = std::move(found);
        }

        // The shells he holds: every Linkshell and Pearlsack in his bags, unbroken
        auto shellsOf(CCharEntity* PPlayer) -> std::set<uint32>
        {
            std::set<uint32> lsids;
            for (uint8 location = 0; location < MAX_CONTAINER_ID; ++location)
            {
                const auto* storage = location == LOC_RECYCLEBIN ? nullptr : PPlayer->getStorage(location);
                for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                {
                    auto* PShell = dynamic_cast<CItemLinkshell*>(storage->GetItem(slot));
                    if (PShell != nullptr && (PShell->getID() == rules::kLinkshell || PShell->getID() == rules::kPearlsack) && PShell->GetLSType() != LSTYPE_BROKEN &&
                        PShell->GetLSID() != 0)
                    {
                        lsids.insert(PShell->GetLSID());
                    }
                }
            }
            return lsids;
        }

        // A Linkpearl of one of his shells in his inventory, not worn, to trade
        auto pearlToTrade(CCharEntity* PPlayer, const std::set<uint32>& shells) -> std::vector<uint8>
        {
            std::vector<uint8> slots;
            const auto*        storage = PPlayer->getStorage(LOC_INVENTORY);
            for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
            {
                auto* PPearl = dynamic_cast<CItemLinkshell*>(storage->GetItem(slot));
                if (PPearl != nullptr && PPearl->getID() == rules::kLinkpearl && PPearl->GetLSType() == LSTYPE_LINKPEARL && shells.contains(PPearl->GetLSID()) &&
                    PPearl->state() != ItemState::Equipped)
                {
                    slots.push_back(slot);
                }
            }
            return slots;
        }

        // The linkshell item in one of her link slots: the game keeps every
        // worn item as a piece of equipment, a linkshell's too, so it is read
        // back as the item it is
        auto linkshellIn(CCharEntity* PChar, const SLOTTYPE slot) -> CItemLinkshell*
        {
            return dynamic_cast<CItemLinkshell*>(reinterpret_cast<CItem*>(PChar->getEquip(slot)));
        }

        // The linkshell item she wears, if she stands: in link slot 1, else 2
        auto wornItem(CCharEntity* PPawn) -> CItemLinkshell*
        {
            for (const auto slot : { SLOT_LINK1, SLOT_LINK2 })
            {
                if (auto* PItem = linkshellIn(PPawn, slot); PItem != nullptr && PItem->GetLSType() != LSTYPE_BROKEN)
                {
                    return PItem;
                }
            }
            return nullptr;
        }

        // She puts a linkshell item of her inventory on, as a player does
        // with the game's own linkshell menu (packet 0x0C4): the game checks
        // the shell, joins her to its members and saves her equipment
        auto wear(CCharEntity* PPawn, const uint8 slot) -> bool
        {
            GP_CLI_COMMAND_GROUP_COMLINK_ACTIVE active{};
            active.a           = 15;
            active.ItemIndex   = slot;
            active.Category    = LOC_INVENTORY;
            active.ActiveFlg   = static_cast<uint8_t>(GP_CLI_COMMAND_GROUP_COMLINK_ACTIVE_ACTIVEFLG::EquipOrCreate);
            active.LinkshellId = static_cast<uint8_t>(GP_CLI_COMMAND_GROUP_COMLINK_ACTIVE_LINKSHELLID::Linkshell1);
            active.process(nullptr, PPawn);
            PPawn->clearPacketList();
            auto* PWorn = wornItem(PPawn);
            return PWorn != nullptr && PWorn->getSlotID() == slot && PWorn->getLocationID() == LOC_INVENTORY;
        }

        auto inPartyWith(const CCharEntity* PPawn, const CCharEntity* PPlayer) -> bool
        {
            return PPawn != nullptr && PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty;
        }

        void say(CCharEntity* PPlayer, const std::string& text)
        {
            PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPlayer, MESSAGE_SYSTEM_3, text);
        }

        // Her affinity with him and the story missions the two have completed
        // together (cardian_party_memory), for one of the world's
        struct Together
        {
            uint32 affinity = 0;
            uint32 missions = 0;
        };

        auto togetherWith(const CCharEntity* PPlayer, const uint32 charid) -> std::optional<Together>
        {
            const auto rset = db::preparedStmt("SELECT m.affinity, m.missions FROM cardian_party_memory m JOIN cardian_census x ON x.charid = m.pawn_charid "
                                               "WHERE m.player_charid = ? AND m.pawn_charid = ? AND x.recruited = 0",
                                               PPlayer->id, charid);
            if (!rset || !rset->next())
            {
                return std::nullopt;
            }
            return Together{ rset->get<uint32>("affinity"), rset->get<uint32>("missions") };
        }

        auto lockAffinity() -> uint32
        {
            return settings::get<uint32>("pawn.PEARL_AFFINITY");
        }

        auto lockMissions() -> uint32
        {
            return settings::get<uint32>("pawn.PEARL_MISSIONS");
        }

        // The world's who qualify as his recruits, wearing no pearl: by charid
        auto recruitsOf(const CCharEntity* PPlayer) -> std::vector<uint32>
        {
            std::vector<uint32> out;
            const auto          rset = db::preparedStmt("SELECT m.pawn_charid FROM cardian_party_memory m JOIN cardian_census x ON x.charid = m.pawn_charid "
                                                        "JOIN chars c ON c.charid = m.pawn_charid "
                                                        "WHERE m.player_charid = ? AND x.recruited = 0 AND m.affinity >= ? AND m.missions >= ? ORDER BY c.charname",
                                                        PPlayer->id, lockAffinity(), lockMissions());
            while (rset && rset->next())
            {
                if (const auto charid = rset->get<uint32>("pawn_charid"); !worn.contains(charid))
                {
                    out.push_back(charid);
                }
            }
            return out;
        }

        auto zoneName(const uint16 zoneId) -> std::string
        {
            auto*       PZone = zoneId != 0 ? zoneutils::GetZone(static_cast<xi::ZoneId>(zoneId)) : nullptr;
            std::string name  = PZone != nullptr ? PZone->getName() : std::string{};
            std::ranges::replace(name, '_', ' ');
            return name;
        }

        // ---- the page ----------------------------------------------------------

        // A member as the page shows her
        auto rowOf(CCharEntity* PPlayer, const uint32 charid, const Member kind, const std::set<uint32>& shells) -> std::optional<cl_club_member>
        {
            auto row    = make<cl_club_member>();
            row.cardian = charid;
            row.kind    = static_cast<uint8_t>(kind);

            const auto* PPawn = pawn::findPawn(charid);
            if (PPawn != nullptr)
            {
                setText(row.name, PPawn->getName());
                row.mainJob   = static_cast<uint8_t>(PPawn->GetMJob());
                row.mainLevel = PPawn->GetMLevel();
                row.subJob    = static_cast<uint8_t>(PPawn->GetSJob());
                row.subLevel  = PPawn->GetSLevel();
                row.zone      = static_cast<uint16_t>(PPawn->getZone());
                for (std::size_t job = 1; job < std::min<std::size_t>(MAX_JOBTYPE, std::size(row.levels)); ++job)
                {
                    row.levels[job] = PPawn->jobs.job[job];
                }
                row.flags |= CL_CLUB_STANDING | CL_CLUB_ONLINE;
                if (inPartyWith(PPawn, PPlayer))
                {
                    row.flags |= CL_CLUB_IN_PARTY;
                }
            }
            else
            {
                const auto rset = db::preparedStmt("SELECT c.charname, c.pos_zone, s.mjob, s.mlvl, s.sjob, s.slvl, "
                                                   "EXISTS (SELECT 1 FROM accounts_sessions o WHERE o.charid = c.charid) AS online "
                                                   "FROM chars c JOIN char_stats s ON s.charid = c.charid WHERE c.charid = ?",
                                                   charid);
                if (!rset || !rset->next())
                {
                    return std::nullopt;
                }
                setText(row.name, rset->get<std::string>("charname"));
                row.mainJob   = rset->get<uint8>("mjob");
                row.mainLevel = rset->get<uint8>("mlvl");
                row.subJob    = rset->get<uint8>("sjob");
                row.subLevel  = rset->get<uint8>("slvl");
                row.zone      = rset->get<uint16>("pos_zone");
                if (rset->get<uint8>("online") != 0)
                {
                    row.flags |= CL_CLUB_ONLINE;
                }
                if (const auto jobs = db::preparedStmt("SELECT * FROM char_jobs WHERE charid = ?", charid); jobs && jobs->next())
                {
                    for (std::size_t job = 1; job < kJobColumns.size() && job < std::size(row.levels); ++job)
                    {
                        row.levels[job] = jobs->get<uint8>(std::string(kJobColumns[job]));
                    }
                }
            }
            setText(row.zoneName, zoneName(row.zone));

            const auto rank = pawn::errands::rankOf(charid);
            row.nation      = rank.nation;
            row.rank        = rank.rank;
            row.rankCap     = pawn::errands::rankCap(rank.nation, PPlayer);

            const auto pearl = pearlOf(charid);
            const bool his   = pearl.has_value() && shells.contains(pearl->lsid);
            if (his)
            {
                row.flags |= CL_CLUB_PEARL;
            }

            const auto errand = pawn::errands::viewOf(charid);
            if (errand.has_value())
            {
                row.errand       = static_cast<uint8_t>(errand->kind);
                row.errandState  = static_cast<uint8_t>(errand->state == cardian::errand::State::Back ? cardian::errand::State::Returning : errand->state);
                row.secondsLeft  = errand->secondsLeft;
                row.secondsTotal = errand->secondsTotal;
                row.missionsDone = errand->missionsDone;
                row.missions     = errand->missions;
                row.errandZone   = errand->zone;
                setText(row.errandZoneName, zoneName(errand->zone));
                setText(row.errandTitle, errand->title);
            }

            // one of the world's: what the two have done together
            if (kind == Member::Wild || kind == Member::Recruit)
            {
                if (const auto together = togetherWith(PPlayer, charid); together.has_value())
                {
                    row.affinity         = static_cast<uint16_t>(std::min<uint32>(together->affinity, UINT16_MAX));
                    row.missionsTogether = static_cast<uint16_t>(std::min<uint32>(together->missions, UINT16_MAX));
                }
            }

            using cardian::errand::Kind;
            if (kind == Member::Alt || kind == Member::Owned)
            {
                row.can2 |= CL_CAN2_PROFESSIONS;
                if (!errand.has_value() && pawn::professions::hasProfession(charid, CL_PROF_FISHING))
                {
                    row.can2 |= CL_CAN2_SEND_FISHING;
                }
                if (pawn::professions::hasReport(charid))
                {
                    row.can2 |= CL_CAN2_REPORT;
                }
            }
            if (kind != Member::Recruit)
            {
                if (errand.has_value())
                {
                    if (errand->state == cardian::errand::State::Going || errand->state == cardian::errand::State::Away)
                    {
                        row.can |= CL_CAN_CALL_BACK;
                    }
                }
                else
                {
                    if ((row.flags & CL_CLUB_IN_PARTY) == 0)
                    {
                        row.can |= CL_CAN_INVITE;
                    }
                    if (cardian::errand::offered(Kind::Quest, kind))
                    {
                        row.can |= CL_CAN_SEND_QUEST;
                    }
                    if (cardian::errand::offered(Kind::Rank, kind) && row.rank < row.rankCap)
                    {
                        row.can |= CL_CAN_SEND_RANK;
                    }
                    if (cardian::errand::offered(Kind::Gear, kind))
                    {
                        row.can |= CL_CAN_SEND_GEAR;
                        row.gearWhy = pawn::errands::gearRefusal(charid);
                    }
                }
            }
            if (his)
            {
                row.can |= CL_CAN_BREAK_PEARL;
            }
            else if (!pearl.has_value() && kind != Member::Wild)
            {
                row.can |= CL_CAN_GIVE_PEARL;
            }
            return row;
        }

        // His club, then the world's who qualify as his recruits; his shell,
        // and with none the vendor of the city he stands in
        void club(CCharEntity* PChar, const cl_club& ask, Reply& reply)
        {
            read();
            const auto shells = shellsOf(PChar);
            auto       answer = ask;
            answer.count      = 0;
            answer.shell      = shells.empty() ? 0 : 1;
            answer.pearls     = static_cast<uint8_t>(std::min<std::size_t>(pearlToTrade(PChar, shells).size(), UINT8_MAX));
            if (shells.empty() && PChar->loc.zone != nullptr && (PChar->loc.zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown)
            {
                if (const auto vendor = rules::vendorOf(static_cast<uint8_t>(PChar->loc.zone->GetRegionID())); vendor.has_value())
                {
                    setText(answer.vendor, vendor->name);
                    setText(answer.vendorZone, vendor->zone);
                }
            }
            const auto send = [&](const uint32 charid, const Member kind)
            {
                if (answer.count == UINT8_MAX)
                {
                    return;
                }
                if (auto row = rowOf(PChar, charid, kind, shells); row.has_value())
                {
                    reply.more(*row);
                    ++answer.count;
                }
            };
            for (const auto& [charid, kind] : membersOf(PChar))
            {
                send(charid, kind);
            }
            for (const auto charid : recruitsOf(PChar))
            {
                send(charid, Member::Recruit);
            }
            reply.finish(answer, CL_S_OK);
        }

        // ---- a recruit comes for her pearl (OPEN_ISSUES #415) ----------------------

        // Her yes carried out: she comes to him for the pearl in no party --
        // a trade needs none -- and goes back to what she was doing once it
        // is hers, or once she has waited long enough. While she comes the
        // crowd's clocks leave her be (the world's hold: keepFor, bringTo)
        enum class Leg : uint8
        {
            Free,    // not set out: her fight first, or a KO
            Trek,    // walking the zone lines to him (a travel order meeting him)
            Trip,    // out of sight, to come in near him at `due`
            Here,    // in his zone: walking up to him, then waiting there
            Leaving, // one from elsewhere walking off, to fade
        };

        struct Visit
        {
            uint32                    playerCharID = 0;
            Leg                       leg          = Leg::Free;
            bool                      local        = false; // standing in his zone when she set out: she goes back on foot
            timer::time_point         since{};              // when she set out
            timer::time_point         due{};                // Trip: when she comes in; Here, waiting: when she gives up; Leaving: when she fades regardless
            std::optional<position_t> spot;                 // Here: the point she waits at; Leaving: where she walks off to
            bool                      waiting = false;      // Here: at her point, facing him
            bool                      greeted = false;      // Here: she has said she is here
            uint16                    from    = 0;          // the zone she set out from, which she leaves toward
            timer::time_point         hereSince{};          // Here: when her walk up to him began, for its deadline
            position_t                himAt{};              // Here: where he stood when her point was laid
            std::optional<bool>       tookHold;             // the visit put the world's hold on her (else one held her already)
            bool                      pearlTaken = false;   // his pearl is hers: she goes back on the next step
        };
        std::unordered_map<uint32, Visit> visits; // by her charid

        // A tell from her by name, so one out of sight can speak too
        void tellFrom(CCharEntity* PPlayer, const uint32 charid, const std::string_view line)
        {
            PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(nameOf(charid), PPlayer->getZone(), MESSAGE_TELL, std::string(line));
        }

        // The world's hold on her while she comes (the crowd's clocks leave
        // her be), noting whether the visit took it: one a contract of his
        // held already stays held after
        void holdHer(const uint32 charid, Visit& visit, const CCharEntity* PPlayer)
        {
            if (!visit.tookHold.has_value())
            {
                visit.tookHold = pawn::world::holderOf(charid) == 0;
            }
            pawn::world::keepFor(charid, PPlayer->id);
        }

        auto bringHer(const uint32 charid, Visit& visit, const CCharEntity* PPlayer, const position_t& at) -> bool
        {
            if (!visit.tookHold.has_value())
            {
                visit.tookHold = pawn::world::holderOf(charid) == 0;
            }
            return pawn::world::bringTo(charid, PPlayer->id, static_cast<uint16>(PPlayer->getZone()), at);
        }

        // The visit over: her orders let go, one from elsewhere faded to
        // stand next at her seat, and the world's again unless a hold of
        // his held her before the visit
        void endVisit(const uint32 charid, const Visit& visit)
        {
            pawn::clearWalkOrder(charid);
            pawn::clearTravelOrder(charid);
            if (!visit.local)
            {
                pawn::world::fadeBody(charid);
            }
            if (visit.tookHold.value_or(false))
            {
                pawn::world::endHold(charid);
            }
        }

        // A point on his zone's mesh `yalms` from `from` toward `toward`, the
        // way walkable in a line; nullopt when the mesh gives none
        auto meshPointToward(CZone* PZone, const position_t& from, const position_t& toward, const float yalms) -> std::optional<position_t>
        {
            auto* navMesh = PZone != nullptr ? PZone->navMesh() : nullptr;
            const float dx = toward.x - from.x;
            const float dz = toward.z - from.z;
            const float d  = std::sqrt(dx * dx + dz * dz);
            if (navMesh == nullptr || d < 0.01f)
            {
                return std::nullopt;
            }
            position_t aim = from;
            aim.x          = from.x + dx / d * yalms;
            aim.z          = from.z + dz / d * yalms;
            const std::optional<position_t> reach = navMesh->findFurthestValidPoint(from, aim);
            if (!reach.has_value())
            {
                return std::nullopt;
            }
            const std::optional<position_t> ground = navMesh->findClosestValidPoint(*reach);
            return ground.has_value() ? ground : reach;
        }

        // She goes back: one standing in his zone when she set out is the
        // world's again where she stands, her seat's pulls taking her home;
        // one from elsewhere walks off toward the zone line on her way home,
        // or straight away from him with none, to fade once out of his sight
        // (Leaving). False when the visit is over
        auto leave(const uint32 charid, Visit& visit, const CCharEntity* PPlayer, const std::string_view why) -> bool
        {
            pawn::clearTravelOrder(charid);
            const auto* PPawn = pawn::findPawn(charid);
            if (!visit.local && PPawn != nullptr && PPawn->loc.zone == PPlayer->loc.zone && !PPawn->isDead())
            {
                std::optional<position_t> away;
                if (visit.from != 0 && visit.from != static_cast<uint16>(PPawn->getZone()))
                {
                    if (const auto hop = pawn::travel::nextHop(PPawn->getZone(), static_cast<xi::ZoneId>(visit.from), PPawn->loc.p); hop.has_value())
                    {
                        away = hop->walkTo;
                    }
                }
                if (!away.has_value())
                {
                    away = meshPointToward(PPawn->loc.zone, PPawn->loc.p,
                                           position_t(2 * PPawn->loc.p.x - PPlayer->loc.p.x, PPawn->loc.p.y, 2 * PPawn->loc.p.z - PPlayer->loc.p.z, 0, 0),
                                           rules::kVisitLeaveTo);
                }
                if (away.has_value())
                {
                    ShowInfoFmt("club: {} goes back from {} ({}), walking off to ({:.1f}, {:.1f}, {:.1f}) {}", nameOf(charid), PPlayer->getName(), why, away->x, away->y, away->z,
                                visit.from != 0 && visit.from != static_cast<uint16>(PPawn->getZone()) ? fmt::format("toward zone {}", visit.from) : std::string("away from him"));
                    pawn::setWalkOrder(charid, *away, pawn::kErrandWalker);
                    visit.leg  = Leg::Leaving;
                    visit.spot = away;
                    visit.due  = timer::now() + std::chrono::seconds(rules::kVisitLeaveSeconds);
                    return true;
                }
            }
            ShowInfoFmt("club: {} goes back from {} ({})", nameOf(charid), PPlayer->getName(), why);
            endVisit(charid, visit);
            return false;
        }

        // ---- the pearl ---------------------------------------------------------

        // She puts on the pearl he traded her. One of the world's -- the
        // recruit -- is held for him from then on, where she stands, and
        // stays wild
        void putOn(CCharEntity* PPlayer, CCharEntity* PPawn, const uint8 slot, const bool joining)
        {
            const auto name = PPawn->getName();
            if (!wear(PPawn, slot))
            {
                ShowErrorFmt("club: {} took {}'s linkpearl but the game would not let her put it on (slot {})", name, PPlayer->getName(), slot);
                say(PPlayer, fmt::format("{} takes the linkpearl.", name));
                read();
                return;
            }
            read();
            // Come for it, she goes back to what she was doing; given in his
            // party, his linkshell's pearl keeps her for him where she stands
            if (const auto visit = visits.find(PPawn->id); visit != visits.end())
            {
                visit->second.pearlTaken = true; // her visit's next step takes her back (advance)
            }
            else if (joining)
            {
                pawn::world::keepFor(PPawn->id, PPlayer->id);
            }
            ShowInfoFmt("club: {} trades {} ({}) a linkpearl, and she puts it on{}", PPlayer->getName(), name, PPawn->id,
                        joining ? ": she joins his linkshell, wild as ever" : "");
            say(PPlayer, joining ? fmt::format("{} puts on the linkpearl and joins the linkshell.", name) : fmt::format("{} puts on the linkpearl.", name));
        }

        // ---- the game's own trade ------------------------------------------------

        // A trade window he opened with her, as the game shows it her: the
        // trade packets it pushes her (0x021 to 0x023), read as her client
        // would read them
        struct Trade
        {
            uint32                                         partner   = 0;     // his charid
            bool                                           open      = false; // she accepted: the window is open
            bool                                           confirmed = false; // he pressed Trade on what he offers now
            std::array<rules::Offered, rules::kTradeSlots> slots{};
        };
        std::unordered_map<uint32, Trade> trades; // by her charid

        // Her answer, as her client would send it (packet 0x033)
        void answerTrade(CCharEntity* PPawn, const GP_CLI_COMMAND_TRADE_RES_KIND kind)
        {
            GP_CLI_COMMAND_TRADE_RES answer{};
            answer.Kind = std::to_underlying(kind);
            if (answer.validate(nullptr, PPawn).valid())
            {
                answer.process(nullptr, PPawn);
            }
            PPawn->clearPacketList();
        }

        // Her word to him: in his party she says it in party chat, which every
        // real player of the party hears; else in a tell to him
        void speak(CCharEntity* PPlayer, const CCharEntity* PPawn, const std::string_view line)
        {
            if (!inPartyWith(PPawn, PPlayer))
            {
                PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPawn, MESSAGE_TELL, std::string(line));
                return;
            }
            for (auto* PMember : PPlayer->PParty->members)
            {
                if (auto* PChar = dynamic_cast<CCharEntity*>(PMember); PChar != nullptr && !pawn::isPawn(PChar))
                {
                    PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPawn, MESSAGE_PARTY, std::string(line));
                }
            }
        }

        // His shells for a trade with her: the ones he holds, and the one whose
        // pearl she wears when another character of his account holds it --
        // a member of his linkshell all the same (memberOf)
        auto shellsHere(CCharEntity* PPlayer, const uint32 charid) -> std::set<uint32>
        {
            auto mine = shellsOf(PPlayer);
            if (const auto pearl = pearlOf(charid); pearl.has_value() && pearl->accid == pawn::ownerAccountOf(PPlayer))
            {
                mine.insert(pearl->lsid);
            }
            return mine;
        }

        // How many trades of his she has declined for no reason of the
        // trade's own (a stranger's, one short of the lock): by his charid
        // and hers, so his next try hears another line
        std::unordered_map<uint64, uint32> brushedOff;

        // She cancels the request or the open window, and says why
        void declineTrade(CCharEntity* PPawn, CCharEntity* PPlayer, const rules::Verdict verdict)
        {
            answerTrade(PPawn, GP_CLI_COMMAND_TRADE_RES_KIND::Cancell);
            uint32 tries = 0;
            if (verdict == rules::Verdict::NotTrading || verdict == rules::Verdict::TooSoon)
            {
                tries = brushedOff[(static_cast<uint64>(PPlayer->id) << 32) | PPawn->id]++;
            }
            speak(PPlayer, PPawn, rules::declineLine(verdict, PPawn->id, tries));
            ShowInfoFmt("club: {} declines {}'s trade ({})", PPawn->getName(), PPlayer->getName(), std::to_underlying(verdict));
        }

        // The slot of her inventory holding a Linkpearl of this shell, not worn
        auto pearlIn(CCharEntity* PPawn, const uint32 lsid) -> std::optional<uint8>
        {
            const auto* storage = PPawn->getStorage(LOC_INVENTORY);
            for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
            {
                auto* PPearl = dynamic_cast<CItemLinkshell*>(storage->GetItem(slot));
                if (PPearl != nullptr && PPearl->getID() == rules::kLinkpearl && PPearl->GetLSID() == lsid && PPearl->GetLSType() == LSTYPE_LINKPEARL &&
                    PPearl->state() != ItemState::Equipped)
                {
                    return slot;
                }
            }
            return std::nullopt;
        }

        // Her pearl of this shell broken as the holder breaks a member's
        // pearl, then thrown away. Standing, the game's own break
        // (CLinkshell::RemoveMemberByName): off her, out of the shell's
        // members, its pearls hers broken; then every broken one out of her
        // bags. Not standing, the same on her rows
        void breakHers(const uint32 charid, const uint32 lsid)
        {
            if (auto* PPawn = pawn::findPawn(charid); PPawn != nullptr)
            {
                if (auto* PShell = linkshell::GetLinkshell(lsid); PShell != nullptr)
                {
                    PShell->RemoveMemberByName(PPawn->getName(), LSTYPE_LINKSHELL);
                }
                for (const auto slot : { SLOT_LINK1, SLOT_LINK2 })
                {
                    if (auto* PItem = linkshellIn(PPawn, slot); PItem != nullptr && PItem->GetLSID() == lsid)
                    {
                        PPawn->clearEquip(slot); // a pearl the shell's members did not list (never loaded): off her all the same
                    }
                }
                uint32 thrown = 0;
                for (uint8 location = 0; location < MAX_CONTAINER_ID; ++location)
                {
                    auto* storage = PPawn->getStorage(location);
                    for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
                    {
                        auto* PItem = dynamic_cast<CItemLinkshell*>(storage->GetItem(slot));
                        if (PItem == nullptr || PItem->GetLSID() != lsid || PItem->getID() == rules::kLinkshell)
                        {
                            continue;
                        }
                        auto transaction = ItemClaimTransaction::start(PPawn);
                        if (transaction && transaction->take(location, slot, PItem->getQuantity()) && transaction->commit())
                        {
                            ++thrown;
                        }
                    }
                }
                charutils::SaveCharEquip(PPawn);
                PPawn->clearPacketList();
                ShowInfoFmt("club: {}'s pearl of linkshell {} is broken; {} thrown away", PPawn->getName(), lsid, thrown);
                return;
            }
            // Her rows: the link slots that wear a pearl of the shell, and every
            // pearl or sack of it in her bags
            std::vector<std::pair<uint8, uint8>> gone; // (location, slot)
            if (const auto rset = db::preparedStmt("SELECT location, slot, extra FROM char_inventory WHERE charid = ? AND itemId IN (?, ?)", charid,
                                                   rules::kPearlsack, rules::kLinkpearl))
            {
                while (rset->next())
                {
                    uint8 extra[CItem::extra_size]{};
                    db::extractFromBlob(rset, "extra", extra);
                    if (rules::lsidOf(extra, sizeof(extra)) == lsid)
                    {
                        gone.emplace_back(rset->get<uint8>("location"), rset->get<uint8>("slot"));
                    }
                }
            }
            for (const auto& [location, slot] : gone)
            {
                db::preparedStmt("DELETE FROM char_equip WHERE charid = ? AND containerid = ? AND slotid = ? AND equipslotid IN (?, ?)", charid, location, slot,
                                 rules::kLinkSlot1, rules::kLinkSlot2);
                db::preparedStmt("DELETE FROM char_inventory WHERE charid = ? AND location = ? AND slot = ? LIMIT 1", charid, location, slot);
            }
            db::preparedStmt("UPDATE accounts_sessions SET linkshellid1 = 0, linkshellrank1 = 0 WHERE charid = ? AND linkshellid1 = ?", charid, lsid);
            db::preparedStmt("UPDATE accounts_sessions SET linkshellid2 = 0, linkshellrank2 = 0 WHERE charid = ? AND linkshellid2 = ?", charid, lsid);
            ShowInfoFmt("club: {}'s pearl of linkshell {} is broken where she is saved; {} thrown away", nameOf(charid), lsid, gone.size());
        }

        auto breakPearl(CCharEntity* PPlayer, const uint32 charid) -> uint16
        {
            const auto shells = shellsOf(PPlayer);
            const auto pearl  = pearlOf(charid);
            if (!pearl.has_value() || !shells.contains(pearl->lsid))
            {
                return CL_S_NO_PEARL;
            }
            const auto member = memberOf(PPlayer, charid);
            const bool wild   = member == Member::Wild;
            const auto name   = nameOf(charid);
            breakHers(charid, pearl->lsid);
            read();
            if (wild)
            {
                // Out of his club: her errand ends, and, out of his party and
                // held by no contract of his, she is the world's again where
                // she stands. In his party she stays in it, under whatever
                // brought her there, until she leaves it
                pawn::errands::drop(charid, "her pearl is broken");
                if (!inPartyWith(pawn::findPawn(charid), PPlayer) && !pawn::finder::openContractOf(charid).has_value())
                {
                    pawn::world::endHold(charid);
                }
            }
            ShowInfoFmt("club: {} breaks {}'s ({}) pearl{}", PPlayer->getName(), name, charid, wild ? ": she leaves his linkshell for the wild" : "");
            say(PPlayer, wild ? fmt::format("{}'s linkpearl is broken. {} leaves the linkshell.", name, name) : fmt::format("{}'s linkpearl is broken.", name));
            return CL_S_OK;
        }

        void pearl(CCharEntity* PChar, const cl_pearl& ask, Reply& reply)
        {
            reply.finish(ask, breakPearl(PChar, ask.cardian));
        }

        // ---- a recruit asked to join ---------------------------------------------

        // A yes given, carried out once she has thought it over: by her charid
        struct Asked
        {
            uint32               playerCharID = 0;
            realtime::time_point due{};
        };
        std::unordered_map<uint32, Asked> asked;

        void askRecruit(CCharEntity* PChar, const cl_club_recruit& ask, Reply& reply)
        {
            auto answer = ask;
            if (!isRecruit(PChar, ask.cardian))
            {
                reply.finish(answer, CL_S_NOT_QUALIFIED);
                return;
            }
            // Nothing to come for without a linkpearl of his in his bags
            if (const auto shells = shellsOf(PChar); shells.empty())
            {
                reply.finish(answer, CL_S_NO_LINKSHELL);
                return;
            }
            else if (pearlToTrade(PChar, shells).empty())
            {
                reply.finish(answer, CL_S_NO_PEARL_TO_GIVE);
                return;
            }
            // One who qualifies says yes, whatever she is doing, and comes for
            // the pearl once she has thought it over (a visit); in his party
            // already, or on her way, there is nothing more to carry out
            const bool withHim = inPartyWith(pawn::findPawn(ask.cardian), PChar);
            const bool coming  = visits.contains(ask.cardian);
            answer.decideMs    = static_cast<uint32_t>(xirand::GetRandomNumber(static_cast<int>(rules::kDecideMinMs), static_cast<int>(rules::kDecideMaxMs) + 1));
            setText(answer.line, std::string(rules::joinYes(ask.cardian)));
            if (!withHim && !coming)
            {
                asked[ask.cardian] = Asked{ PChar->id, realtime::now() + std::chrono::milliseconds(answer.decideMs) };
            }
            ShowInfoFmt("club: {} asks {} to join his linkshell: yes{}", PChar->getName(), nameOf(ask.cardian),
                        withHim ? ", with him already" : coming ? ", on her way already" : "");
            reply.finish(answer, CL_S_OK);
        }

        // Her yes carried out, once its beat is over: she sets out to him for
        // the pearl (a visit), whatever she was doing
        void carryOutYeses(CZone* PZone)
        {
            std::vector<std::pair<uint32, Asked>> due;
            for (const auto& [charid, a] : asked)
            {
                if (realtime::now() >= a.due)
                {
                    due.emplace_back(charid, a);
                }
            }
            for (const auto& [charid, a] : due)
            {
                auto* PPlayer = zoneutils::GetChar(a.playerCharID);
                if (PPlayer == nullptr)
                {
                    if (!pawn::players::online(a.playerCharID))
                    {
                        asked.erase(charid); // he has left the world; loading between zones, he is waited for
                    }
                    continue;
                }
                if (PPlayer->loc.zone != PZone)
                {
                    continue;
                }
                asked.erase(charid);
                const auto* PPawn = pawn::findPawn(charid);
                if (!isRecruit(PPlayer, charid) || inPartyWith(PPawn, PPlayer))
                {
                    continue; // his pearl given meanwhile, or in his party already: nothing to come for
                }
                const bool  here  = PPawn != nullptr && PPawn->loc.zone == PPlayer->loc.zone;
                visits[charid]    = Visit{ .playerCharID = PPlayer->id, .local = here, .since = timer::now() };
                ShowInfoFmt("club: {} sets out to {} for his linkpearl ({})", nameOf(charid), PPlayer->getName(),
                            PPawn == nullptr ? std::string("faded") : here ? std::string("in his zone") : fmt::format("in zone {}", static_cast<uint16>(PPawn->getZone())));
            }
        }

        // Zone lines between two zones; max for no route
        auto zonesBetween(const uint16 from, const uint16 to) -> uint32
        {
            if (from == to)
            {
                return 0;
            }
            const auto hops = pawn::travel::hops({ static_cast<xi::ZoneId>(to) });
            const auto it   = hops.find(from);
            return it != hops.end() ? it->second : std::numeric_limits<uint32>::max();
        }

        // Where a faded body was saved: her zone while out of sight
        auto savedZoneOf(const uint32 charid) -> uint16
        {
            const auto rset = db::preparedStmt("SELECT pos_zone FROM chars WHERE charid = ?", charid);
            return rset && rset->next() ? rset->get<uint16>("pos_zone") : 0;
        }

        // Where one out of sight comes in: kVisitArrive yalms from him, behind
        // him first and round him after, on his zone's mesh; beside him when
        // the mesh gives nowhere
        auto arrivalNear(const CCharEntity* PPlayer) -> position_t
        {
            for (const float turn : { 1.0f, 0.75f, 1.25f, 0.5f, 1.5f, 0.0f })
            {
                const auto aim   = nearPosition(PPlayer->loc.p, rules::kVisitArrive, turn * std::numbers::pi_v<float>);
                const auto reach = meshPointToward(PPlayer->loc.zone, PPlayer->loc.p, aim, rules::kVisitArrive);
                if (reach.has_value() && distance(PPlayer->loc.p, *reach) >= rules::kVisitArrive / 2)
                {
                    return *reach;
                }
            }
            return PPlayer->loc.p;
        }

        // Her point beside him: her ring's distance from him (visitRing), on
        // her side of him, turned round him a step at a time off any point
        // another who comes to him waits at, so no two stand on one spot
        auto spotBeside(const uint32 charid, const CCharEntity* PPawn, const CCharEntity* PPlayer) -> position_t
        {
            const position_t& him  = PPlayer->loc.p;
            const float       ring = rules::visitRing(charid);
            const float       dx   = PPawn->loc.p.x - him.x;
            const float       dz   = PPawn->loc.p.z - him.z;
            const float       base = dx * dx + dz * dz > 0.25f ? std::atan2(dz, dx) : static_cast<float>(charid % 8) * std::numbers::pi_v<float> / 4;
            for (const int step : { 0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6 })
            {
                const float angle = base + static_cast<float>(step) * rules::kVisitSpread;
                position_t  aim   = him;
                aim.x             = him.x + std::cos(angle) * ring;
                aim.z             = him.z + std::sin(angle) * ring;
                const auto at     = meshPointToward(PPlayer->loc.zone, him, aim, ring);
                if (!at.has_value() || distance(him, *at) < ring * 0.6f)
                {
                    continue; // the mesh cuts it short: a wall that way
                }
                const bool taken = std::ranges::any_of(visits, [&](const auto& entry)
                                                       { return entry.first != charid && entry.second.playerCharID == PPlayer->id && entry.second.leg == Leg::Here &&
                                                                entry.second.spot.has_value() && distance(*entry.second.spot, *at) < rules::kVisitApart; });
                if (!taken)
                {
                    return *at;
                }
            }
            return meshPointToward(PPlayer->loc.zone, him, PPawn->loc.p, ring).value_or(PPawn->loc.p);
        }

        // At his side: an emote at him, text and all, and a tell
        void greet(const uint32 charid, CCharEntity* PPawn, CCharEntity* PPlayer)
        {
            const auto emote = static_cast<Emote>(rules::greetEmote(charid));
            PPawn->loc.zone->PushPacket(PPawn, CHAR_INRANGE_SELF, std::make_unique<GP_SERV_COMMAND_MOTIONMES>(PPawn, PPlayer->id, PPlayer->targid, emote, EmoteMode::All, 0));
            tellFrom(PPlayer, charid, rules::imHere(charid));
            ShowInfoFmt("club: {} waits at {}'s side for his linkpearl, and greets him (emote {})", nameOf(charid), PPlayer->getName(), static_cast<uint8>(emote));
        }

        // One visit, a step on his zone's tick (paused, none). False once it
        // is over
        auto advance(const uint32 charid, Visit& visit, CCharEntity* PPlayer) -> bool
        {
            const auto now   = timer::now();
            auto*      PPawn = pawn::findPawn(charid);

            // In his party now -- invited on the way, or after the pearl --
            // the party has her: the visit's own walk and trek let go, nothing
            // faded, the hold left to the party's end (party_finder noteLeft)
            if (inPartyWith(PPawn, PPlayer))
            {
                if (pawn::walkOrderedBy(charid) == pawn::kErrandWalker)
                {
                    pawn::clearWalkOrder(charid);
                }
                pawn::clearTravelOrder(charid);
                ShowInfoFmt("club: {}'s visit ends: she is in {}'s party", nameOf(charid), PPlayer->getName());
                return false;
            }
            if (visit.pearlTaken && visit.leg != Leg::Leaving)
            {
                return leave(charid, visit, PPlayer, "the pearl is hers");
            }

            if (visit.leg == Leg::Leaving)
            {
                const bool gone    = PPawn == nullptr || PPawn->loc.zone != PPlayer->loc.zone;
                const bool unseen  = !gone && distance(PPawn->loc.p, PPlayer->loc.p) >= rules::kVisitFadeAt;
                const bool there   = !gone && visit.spot.has_value() && distance(PPawn->loc.p, *visit.spot) < 3.0f;
                const bool stopped = !gone && !pawn::walkOrderOf(charid).has_value(); // the walk found no way
                if (gone || unseen || there || stopped || now >= visit.due)
                {
                    ShowInfoFmt("club: {} fades on her way home ({})", nameOf(charid),
                                gone ? "out of the zone" : unseen ? "out of his sight" : there ? "at the zone line" : stopped ? "no way on" : "on the clock");
                    endVisit(charid, visit);
                    return false;
                }
                return true;
            }
            if (visit.leg != Leg::Here && now - visit.since >= std::chrono::seconds(rules::kVisitGiveUpSeconds))
            {
                tellFrom(PPlayer, charid, rules::joinLater());
                return leave(charid, visit, PPlayer, "too long on the way");
            }

            switch (visit.leg)
            {
                case Leg::Free:
                {
                    if (PPawn != nullptr && (PPawn->isDead() || (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged())))
                    {
                        return true; // her fight first
                    }
                    if (PPawn != nullptr && PPawn->loc.zone == PPlayer->loc.zone)
                    {
                        holdHer(charid, visit, PPlayer);
                        visit.leg = Leg::Here;
                        return true;
                    }
                    const uint16 from  = PPawn != nullptr ? static_cast<uint16>(PPawn->getZone()) : savedZoneOf(charid);
                    const uint32 zones = zonesBetween(from, static_cast<uint16>(PPlayer->getZone()));
                    visit.from         = from;
                    tellFrom(PPlayer, charid, rules::headingYourWay(charid));
                    if (PPawn != nullptr && zones <= rules::kTrekZones && pawn::orderTravel(charid, static_cast<uint16>(PPlayer->getZone()), PPlayer->id))
                    {
                        holdHer(charid, visit, PPlayer);
                        visit.leg = Leg::Trek;
                        ShowInfoFmt("club: {} walks to {} from zone {}, {} zone lines away", nameOf(charid), PPlayer->getName(), from, zones);
                        return true;
                    }
                    visit.leg = Leg::Trip;
                    visit.due = now + std::chrono::seconds(rules::tripSeconds(zones));
                    ShowInfoFmt("club: {} comes to {} out of sight from zone {} ({} zone lines away), in {} s", nameOf(charid), PPlayer->getName(), from,
                                zones == std::numeric_limits<uint32>::max() ? std::string("no route") : std::to_string(zones), rules::tripSeconds(zones));
                    return true;
                }
                case Leg::Trek:
                {
                    if (PPawn != nullptr && PPawn->loc.zone == PPlayer->loc.zone)
                    {
                        pawn::clearTravelOrder(charid);
                        visit.leg = Leg::Here;
                        return true;
                    }
                    // Faded on the way, or a trek that found no way: out of sight from here
                    if (PPawn == nullptr || !pawn::travelOrderOf(charid).has_value())
                    {
                        ShowInfoFmt("club: {}'s walk to {} ends out of sight ({})", nameOf(charid), PPlayer->getName(), PPawn == nullptr ? "she faded" : "no way on");
                        pawn::clearTravelOrder(charid);
                        visit.leg = Leg::Trip;
                        visit.due = now + std::chrono::seconds(rules::tripSeconds(rules::kTrekZones));
                    }
                    return true;
                }
                case Leg::Trip:
                {
                    if (now < visit.due)
                    {
                        return true;
                    }
                    if (PPawn != nullptr && PPawn->loc.zone == PPlayer->loc.zone)
                    {
                        holdHer(charid, visit, PPlayer);
                        visit.leg = Leg::Here;
                        return true;
                    }
                    if (PPawn != nullptr)
                    {
                        pawn::world::fadeBody(charid); // stood elsewhere meanwhile: unseen there, she leaves
                    }
                    const auto at = arrivalNear(PPlayer);
                    if (bringHer(charid, visit, PPlayer, at) && pawn::seats::inviteStand(charid))
                    {
                        visit.leg = Leg::Here;
                        return true;
                    }
                    visit.due = now + std::chrono::seconds(5); // the ladder could not stand her now: again shortly, the give-up clock running
                    return true;
                }
                case Leg::Here:
                {
                    if (PPawn == nullptr)
                    {
                        visit.leg = Leg::Trip; // faded under her: in again out of sight
                        visit.due = now;
                        visit.spot.reset();
                        visit.waiting   = false;
                        visit.hereSince = {};
                        return true;
                    }
                    if (PPawn->loc.zone != PPlayer->loc.zone)
                    {
                        pawn::clearWalkOrder(charid); // he moved on: after him again
                        visit.leg   = Leg::Free;
                        visit.local = false;
                        visit.spot.reset();
                        visit.waiting   = false;
                        visit.hereSince = {};
                        return true;
                    }
                    if (PPawn->isDead())
                    {
                        return leave(charid, visit, PPlayer, "knocked out");
                    }
                    if (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged())
                    {
                        return true;
                    }
                    // Her point beside him (spotBeside), laid again whenever he
                    // has moved on from where he stood
                    if (!visit.spot.has_value() || distance(visit.himAt, PPlayer->loc.p) > 3.0f)
                    {
                        visit.spot      = spotBeside(charid, PPawn, PPlayer);
                        visit.himAt     = PPlayer->loc.p;
                        visit.hereSince = now;
                        visit.waiting   = false;
                        pawn::setWalkOrder(charid, *visit.spot, pawn::kErrandWalker);
                    }
                    // At her point -- or as near as she gets: a point the walker
                    // dropped, or a minute and a half walking up -- she waits there
                    const bool atSpot = distance(PPawn->loc.p, *visit.spot) < 0.6f;
                    const bool stuck  = !pawn::walkOrderOf(charid).has_value() || now - visit.hereSince >= std::chrono::seconds(90);
                    if (!visit.waiting && (atSpot || stuck))
                    {
                        if (!atSpot)
                        {
                            pawn::clearWalkOrder(charid);
                            visit.spot = PPawn->loc.p;
                        }
                        visit.waiting = true;
                        visit.due     = now + std::chrono::seconds(rules::kVisitWaitSeconds);
                        PPawn->loc.p.rotation = worldAngle(PPawn->loc.p, PPlayer->loc.p);
                        PPawn->updatemask |= UPDATE_POS;
                        if (!visit.greeted)
                        {
                            visit.greeted = true;
                            greet(charid, PPawn, PPlayer);
                        }
                    }
                    if (!visit.waiting)
                    {
                        return true;
                    }
                    if (const uint8 toward = worldAngle(PPawn->loc.p, PPlayer->loc.p); std::abs(angleDifference(toward, PPawn->loc.p.rotation)) > 8)
                    {
                        PPawn->loc.p.rotation = toward;
                        PPawn->updatemask |= UPDATE_POS;
                    }
                    if (now >= visit.due && !trades.contains(charid))
                    {
                        tellFrom(PPlayer, charid, rules::catchYouLater(charid));
                        return leave(charid, visit, PPlayer, "he did not trade her the pearl in time");
                    }
                    return true;
                }
                case Leg::Leaving:
                    break;
            }
            return true;
        }

        // ---- his idle members loiter ----------------------------------------------

        // His club's members standing in his zone with nothing to do -- not
        // in a party, on a venture, coming for a pearl, walked by him or on
        // a trek, fighting or down, and not one of the world's out in the
        // wild, whom the world's own seats keep -- gather at one of the
        // town's loitering spots (world.h loiterSpots), in a ring facing its
        // middle, and emote at each other as the world's cliques do
        // (WORLD_CHAT_GAP_MIN to _MAX seconds apart, one in three turning to
        // the one she emotes at), until he puts them in a party. In the
        // field they stand where they are. A stopgap until the world's own
        // life reaches them (the user, 2026-10-09)
        struct Lounge
        {
            uint16                                         zone = 0;
            std::optional<position_t>                      centre; // the spot, chosen once while he is in the zone
            float                                          spread = 0.0f;
            std::vector<uint32>                            idle;   // by charid, as last read
            std::unordered_map<uint32, position_t>         seats;  // by her charid: where she stands
            std::unordered_map<uint32, timer::time_point>  faceBackAt;
            timer::time_point                              readAt{};
            timer::time_point                              chatAt{};
        };
        std::unordered_map<uint32, Lounge> lounges; // by his charid

        constexpr auto kLoungeRead = std::chrono::seconds(2);

        // The walk order the lounge gave her, and none other's: an errand's
        // walk or his own is never taken off her
        auto seatedThere(const uint32 charid, const position_t& seat) -> bool
        {
            const auto walk = pawn::walkOrderOf(charid);
            return walk.has_value() && pawn::walkOrderedBy(charid) == pawn::kErrandWalker && distance(*walk, seat) < 0.1f;
        }

        auto loiters(const uint32 charid, const CCharEntity* PPlayer, const Lounge& lounge) -> bool
        {
            const auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr || PPawn->loc.zone != PPlayer->loc.zone || PPawn->isDead() || PPawn->PParty != nullptr ||
                (PPawn->PAI != nullptr && PPawn->PAI->IsEngaged()))
            {
                return false;
            }
            if (visits.contains(charid) || pawn::errands::viewOf(charid).has_value() || pawn::travelOrderOf(charid).has_value() ||
                (pawn::world::isBody(charid) && pawn::world::inTheWild(charid)))
            {
                return false;
            }
            const uint32 by   = pawn::walkOrderedBy(charid);
            const auto   seat = lounge.seats.find(charid);
            return by == 0 || (seat != lounge.seats.end() && seatedThere(charid, seat->second));
        }

        // Let go of her seat: the lounge's walk taken off her, if it is still hers
        void unseat(Lounge& lounge, const uint32 charid)
        {
            if (const auto seat = lounge.seats.find(charid); seat != lounge.seats.end())
            {
                if (seatedThere(charid, seat->second))
                {
                    pawn::clearWalkOrder(charid);
                }
                lounge.seats.erase(seat);
            }
            lounge.faceBackAt.erase(charid);
        }

        void faceToward(CCharEntity* PPawn, const position_t& at)
        {
            if (const uint8 toward = worldAngle(PPawn->loc.p, at); std::abs(angleDifference(toward, PPawn->loc.p.rotation)) > 8)
            {
                PPawn->loc.p.rotation = toward;
                PPawn->updatemask |= UPDATE_POS;
            }
        }

        void loungeTick(CCharEntity* PPlayer)
        {
            const auto now    = timer::now();
            const auto zoneId = static_cast<uint16>(PPlayer->getZone());
            Lounge&    lounge = lounges[PPlayer->id];

            // A new zone, a new spot: the last zone's seats let go where they stand
            if (lounge.zone != zoneId)
            {
                for (const auto charid : std::vector<uint32>(lounge.idle))
                {
                    unseat(lounge, charid);
                }
                lounge = Lounge{ .zone = zoneId };
            }

            if (now >= lounge.readAt)
            {
                lounge.readAt = now + kLoungeRead;
                std::vector<uint32> idle;
                for (const auto& [charid, kind] : pawn::club::membersOf(PPlayer))
                {
                    if (loiters(charid, PPlayer, lounge))
                    {
                        idle.push_back(charid);
                    }
                }
                for (const auto charid : lounge.idle)
                {
                    if (std::ranges::find(idle, charid) == idle.end())
                    {
                        unseat(lounge, charid);
                    }
                }
                std::ranges::sort(idle);
                const bool changed = idle != lounge.idle;
                lounge.idle        = std::move(idle);

                // The spot nearest where they stand, chosen once
                if (!lounge.centre.has_value() && !lounge.idle.empty())
                {
                    const auto spots = pawn::world::loiterSpots(PPlayer->loc.zone);
                    if (spots.empty())
                    {
                        lounge.readAt = now + std::chrono::seconds(30); // the field: nowhere to go
                        return;
                    }
                    const auto* PFirst = pawn::findPawn(lounge.idle.front());
                    const auto  where  = PFirst != nullptr ? PFirst->loc.p : PPlayer->loc.p;
                    const auto  best   = std::ranges::min_element(spots, {}, [&](const pawn::world::Loiter& l) { return distance(l.at, where); });
                    lounge.centre      = best->at;
                    lounge.spread      = best->spread;
                    ShowInfoFmt("club: {}'s idle linkshell members gather at ({:.1f}, {:.1f}, {:.1f}) in {}", PPlayer->getName(), best->at.x, best->at.y, best->at.z,
                                PPlayer->loc.zone->getName());
                }

                // A ring round the spot's middle, one seat each, laid again as
                // the ring changes
                if (changed && lounge.centre.has_value())
                {
                    const auto   n      = lounge.idle.size();
                    const float  radius = n <= 1 ? 0.0f : std::clamp(0.9f + 0.3f * static_cast<float>(n), 1.3f, std::max(1.6f, lounge.spread));
                    const float  turn   = static_cast<float>(PPlayer->id % 16) * std::numbers::pi_v<float> / 8;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float angle = turn + 2 * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(std::max<std::size_t>(n, 1));
                        position_t  aim   = *lounge.centre;
                        aim.x += std::cos(angle) * radius;
                        aim.z += std::sin(angle) * radius;
                        const auto seat   = radius > 0.0f ? meshPointToward(PPlayer->loc.zone, *lounge.centre, aim, radius).value_or(*lounge.centre) : *lounge.centre;
                        const auto charid = lounge.idle[i];
                        if (const auto had = lounge.seats.find(charid); had != lounge.seats.end() && seatedThere(charid, had->second))
                        {
                            pawn::clearWalkOrder(charid);
                        }
                        lounge.seats[charid] = seat;
                        pawn::setWalkOrder(charid, seat, pawn::kErrandWalker);
                    }
                }
            }
            if (!lounge.centre.has_value())
            {
                return;
            }

            // At her seat she faces the middle -- alone, him -- or the one she
            // turned to, until it is time to turn back
            std::vector<uint32> seated;
            for (const auto charid : lounge.idle)
            {
                auto*      PPawn = pawn::findPawn(charid);
                const auto seat  = lounge.seats.find(charid);
                if (PPawn == nullptr || seat == lounge.seats.end() || distance(PPawn->loc.p, seat->second) > 0.6f)
                {
                    continue;
                }
                seated.push_back(charid);
                if (const auto back = lounge.faceBackAt.find(charid); back != lounge.faceBackAt.end())
                {
                    if (now < back->second)
                    {
                        continue;
                    }
                    lounge.faceBackAt.erase(back);
                }
                faceToward(PPawn, lounge.idle.size() >= 2 ? *lounge.centre : PPlayer->loc.p);
            }

            // One speaker at a time, at another of the ring
            const auto gapMin = settings::get<uint32>("pawn.WORLD_CHAT_GAP_MIN");
            const auto gapMax = settings::get<uint32>("pawn.WORLD_CHAT_GAP_MAX");
            if (seated.size() < 2 || gapMax == 0 || now < lounge.chatAt)
            {
                return;
            }
            lounge.chatAt = now + std::chrono::seconds(xirand::GetRandomNumber(std::min(gapMin, gapMax), gapMax + 1));
            const auto speaker  = seated[static_cast<std::size_t>(xirand::GetRandomNumber(0, static_cast<int>(seated.size())))];
            auto       listener = seated[static_cast<std::size_t>(xirand::GetRandomNumber(0, static_cast<int>(seated.size()) - 1))];
            if (listener == speaker)
            {
                listener = seated.back();
            }
            auto*       PSpeaker  = pawn::findPawn(speaker);
            const auto* PListener = pawn::findPawn(listener);
            if (PSpeaker == nullptr || PListener == nullptr)
            {
                return;
            }
            static constexpr std::array<Emote, 18> kTalk{ Emote::Wave, Emote::Bow, Emote::Salute, Emote::Laugh, Emote::No, Emote::Yes, Emote::Joy, Emote::Cheer, Emote::Clap,
                                                         Emote::Praise, Emote::Smile, Emote::Sigh, Emote::Comfort, Emote::Surprised, Emote::Amazed, Emote::Grin, Emote::Doubt, Emote::Huh };
            const Emote emote = kTalk[static_cast<std::size_t>(xirand::GetRandomNumber(0, static_cast<int>(kTalk.size())))];
            if (xirand::GetRandomNumber(0, 3) == 0)
            {
                faceToward(PSpeaker, PListener->loc.p);
                lounge.faceBackAt[speaker] = now + std::chrono::seconds(xirand::GetRandomNumber(4, 10));
            }
            PSpeaker->loc.zone->PushPacket(PSpeaker, CHAR_INRANGE_SELF, std::make_unique<GP_SERV_COMMAND_MOTIONMES>(PSpeaker, PListener->id, PListener->targid, emote, EmoteMode::Motion, 0));
        }

        // Every real player in this zone: his idle members loiter (paused, none)
        void loungeAll(CZone* PZone)
        {
            if (cardian::pause::isHeld())
            {
                return;
            }
            std::vector<CCharEntity*> players;
            PZone->ForEachChar([&](CCharEntity* PChar)
                               {
                                   if (PChar != nullptr && !pawn::isPawn(PChar))
                                   {
                                       players.push_back(PChar);
                                   }
                               });
            for (auto* PPlayer : players)
            {
                loungeTick(PPlayer);
            }
        }

        // Every visit to a player in this zone, a step each; one whose player
        // has left the world ends where she is
        void advanceVisits(CZone* PZone)
        {
            if (cardian::pause::isHeld())
            {
                return;
            }
            std::vector<uint32> coming;
            for (const auto& entry : visits)
            {
                coming.push_back(entry.first);
            }
            for (const auto charid : coming)
            {
                const auto it = visits.find(charid);
                if (it == visits.end())
                {
                    continue;
                }
                auto* PPlayer = zoneutils::GetChar(it->second.playerCharID);
                if (PPlayer == nullptr)
                {
                    if (pawn::players::online(it->second.playerCharID))
                    {
                        continue; // loading between zones: in no zone for a moment
                    }
                    ShowInfoFmt("club: {}'s visit ends: {} has left the world", nameOf(charid), pawn::seats::nameOf(it->second.playerCharID));
                    endVisit(charid, it->second);
                    visits.erase(it);
                    continue;
                }
                if (PPlayer->loc.zone != PZone)
                {
                    continue;
                }
                if (!advance(charid, it->second, PPlayer))
                {
                    visits.erase(charid);
                }
            }
        }

        // ---- the invite --------------------------------------------------------

        // One of his own, invited from the page: stood first where the game
        // saved her if she has no body, then asked as the game's own invite
        // asks; she accepts by herself, and the alts' rule decides whether
        // she follows, runs to him or holds (pawn.cpp gatherOrHold)
        auto inviteOwn(CCharEntity* PPlayer, const uint32 charid) -> uint16
        {
            if (PPlayer->PParty != nullptr && PPlayer->PParty->GetLeader() != PPlayer)
            {
                return CL_S_NOT_LEADER;
            }
            if (PPlayer->PParty != nullptr && PPlayer->PParty->IsFull())
            {
                return CL_S_PARTY_FULL;
            }
            auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr)
            {
                if (!pawn::seats::has(charid))
                {
                    uint16 zone = 0;
                    if (const auto rset = db::preparedStmt("SELECT pos_zone FROM chars WHERE charid = ?", charid); rset && rset->next())
                    {
                        zone = rset->get<uint16>("pos_zone");
                    }
                    pawn::seats::offerOwned(charid, PPlayer->id, zone);
                }
                if (const auto status = pawn::finder::standFaded(PPlayer, charid); status != CL_S_OK)
                {
                    return status;
                }
                PPawn = pawn::findPawn(charid);
            }
            if (PPawn == nullptr)
            {
                return CL_S_CANNOT_STAND;
            }
            if (PPawn->isDead())
            {
                return CL_S_KNOCKED_OUT;
            }
            if (PPawn->PParty != nullptr)
            {
                return CL_S_IN_A_PARTY;
            }
            if (PPawn->InvitePending.entity.UniqueNo != 0)
            {
                return CL_S_INVITE_PENDING;
            }
            PPawn->InvitePending.entity.UniqueNo = PPlayer->id;
            PPawn->InvitePending.entity.ActIndex = PPlayer->targid;
            PPawn->InvitePending.kind            = PartyKind::Party;
            PPawn->pushPacket<GP_SERV_COMMAND_GROUP_SOLICIT_REQ>(PPawn->id, PPawn->targid, PPlayer->getName(), PartyKind::Party);
            ShowInfoFmt("club: {} invites {} from the linkshell page", PPlayer->getName(), PPawn->getName());
            return CL_S_OK;
        }

        void invite(CCharEntity* PChar, const cl_club_invite& ask, Reply& reply)
        {
            const auto member = memberOf(PChar, ask.cardian);
            if (!member.has_value())
            {
                reply.finish(ask, CL_S_NOT_IN_CLUB);
                return;
            }
            if (pawn::errands::viewOf(ask.cardian).has_value())
            {
                reply.finish(ask, CL_S_ON_ERRAND);
                return;
            }
            if (*member == Member::Wild)
            {
                // Her pearl holds her for him as an open contract does: his
                // invite needs no shout, and the finder's own invite asks her
                std::string line;
                reply.finish(ask, pawn::finder::invite(PChar, ask.cardian, pawn::finder::Goal{}, line));
                return;
            }
            reply.finish(ask, inviteOwn(PChar, ask.cardian));
        }
    } // namespace

    void load()
    {
        read();
        ShowInfoFmt("club: {} linkpearl{} worn of a shell somebody holds", worn.size(), worn.size() == 1 ? "" : "s");
    }

    void registerHandlers()
    {
        cardian::link::handle<cl_club>(club);
        cardian::link::handle<cl_club_invite>(invite);
        cardian::link::handle<cl_pearl>(pearl);
        cardian::link::handle<cl_club_recruit>(askRecruit);
    }

    auto pearlOf(const uint32 charid) -> std::optional<Pearl>
    {
        const auto it = worn.find(charid);
        return it != worn.end() ? std::optional(it->second) : std::nullopt;
    }

    auto isPearled(const uint32 charid) -> bool
    {
        return worn.contains(charid);
    }

    auto isRecruit(const CCharEntity* PPlayer, const uint32 charid) -> bool
    {
        if (PPlayer == nullptr || charid == 0 || worn.contains(charid))
        {
            return false;
        }
        const auto together = togetherWith(PPlayer, charid);
        return together.has_value() && rules::qualifies(together->affinity, together->missions, lockAffinity(), lockMissions());
    }

    auto wearersOf(const uint32 playerCharID) -> std::vector<uint32>
    {
        std::vector<uint32> out;
        for (const auto& [charid, pearl] : worn)
        {
            if (pearl.playerCharID == playerCharID)
            {
                out.push_back(charid);
            }
        }
        return out;
    }

    auto memberOf(const CCharEntity* PPlayer, const uint32 charid) -> std::optional<Member>
    {
        if (PPlayer == nullptr || charid == 0 || charid == PPlayer->id)
        {
            return std::nullopt;
        }
        const uint32 account = pawn::ownerAccountOf(PPlayer);
        const auto   rset    = db::preparedStmt("SELECT c.accid, COALESCE(p.owner_accid, 0) AS owner FROM chars c "
                                                "LEFT JOIN cardian_pawns p ON p.pawn_charid = c.charid WHERE c.charid = ?",
                                                charid);
        if (!rset || !rset->next())
        {
            return std::nullopt;
        }
        if (rset->get<uint32>("accid") == account)
        {
            return Member::Alt;
        }
        if (rset->get<uint32>("owner") == account)
        {
            return Member::Owned;
        }
        // one of the world's: while she wears a pearl of a shell he holds
        if (const auto pearl = pearlOf(charid); pearl.has_value() && pearl->accid == account)
        {
            return Member::Wild;
        }
        return std::nullopt;
    }

    auto membersOf(const CCharEntity* PPlayer) -> std::vector<std::pair<uint32, Member>>
    {
        std::vector<std::pair<uint32, Member>> out;
        if (PPlayer == nullptr)
        {
            return out;
        }
        std::vector<std::pair<std::string, std::pair<uint32, Member>>> named;
        for (const auto& [charid, name] : pawn::accountPawns(PPlayer))
        {
            if (const auto kind = memberOf(PPlayer, charid); kind.has_value())
            {
                named.push_back({ name, { charid, *kind } });
            }
        }
        const uint32 account = pawn::ownerAccountOf(PPlayer);
        for (const auto& [charid, pearl] : worn)
        {
            if (pearl.accid != account || std::ranges::any_of(named, [&](const auto& n) { return n.second.first == charid; }))
            {
                continue;
            }
            if (const auto kind = memberOf(PPlayer, charid); kind.has_value())
            {
                named.push_back({ nameOf(charid), { charid, *kind } });
            }
        }
        std::ranges::sort(named, {}, [](const auto& n) -> const std::string& { return n.first; });
        for (const auto& n : named)
        {
            out.push_back(n.second);
        }
        return out;
    }

    void wearGiven(CCharEntity* PPawn)
    {
        if (PPawn == nullptr || wornItem(PPawn) != nullptr)
        {
            return;
        }
        const auto* storage = PPawn->getStorage(LOC_INVENTORY);
        for (uint8 slot = 1; storage != nullptr && slot <= storage->GetSize(); ++slot)
        {
            auto* PPearl = dynamic_cast<CItemLinkshell*>(storage->GetItem(slot));
            if (PPearl != nullptr && (PPearl->getID() == rules::kLinkpearl || PPearl->getID() == rules::kPearlsack) && PPearl->GetLSType() != LSTYPE_BROKEN)
            {
                if (wear(PPawn, slot))
                {
                    ShowInfoFmt("club: {} puts on the linkpearl she was given", PPawn->getName());
                }
                read();
                return;
            }
        }
    }

    void noteTradePacket(CCharEntity* PPawn, CBasicPacket& packet)
    {
        const auto type = packet.getType();
        if (type == std::to_underlying(PacketS2C::GP_SERV_COMMAND_ITEM_TRADE_REQ))
        {
            trades[PPawn->id] = Trade{ .partner = rules::tradeAsker(packet) };
            return;
        }
        const auto it = trades.find(PPawn->id);
        if (it == trades.end())
        {
            return;
        }
        if (type == std::to_underlying(PacketS2C::GP_SERV_COMMAND_ITEM_TRADE_RES))
        {
            switch (rules::tradeResult(packet))
            {
                case GP_ITEM_TRADE_RES_KIND::Start:
                    it->second.open = true;
                    break;
                case GP_ITEM_TRADE_RES_KIND::Make:
                    it->second.confirmed = true;
                    break;
                default:
                    trades.erase(it); // cancelled, refused or done
                    break;
            }
        }
        else if (type == std::to_underlying(PacketS2C::GP_SERV_COMMAND_ITEM_TRADE_LIST))
        {
            if (const auto offered = rules::offeredSlot(packet); offered.slot < rules::kTradeSlots)
            {
                it->second.slots[offered.slot] = offered.offered;
                it->second.confirmed           = false; // a changed offer takes back his Trade, as the game does
            }
        }
    }

    void tick(CZone* PZone)
    {
        carryOutYeses(PZone);
        advanceVisits(PZone);
        loungeAll(PZone);

        std::vector<uint32> trading;
        for (const auto& entry : trades)
        {
            trading.push_back(entry.first);
        }
        for (const auto charid : trading)
        {
            const auto it = trades.find(charid);
            if (it == trades.end())
            {
                continue;
            }
            auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr)
            {
                trades.erase(it);
                continue;
            }
            if (PPawn->loc.zone != PZone)
            {
                continue;
            }
            auto* PPlayer = PPawn->tradePartner();
            if (PPlayer == nullptr || PPlayer->id != it->second.partner)
            {
                trades.erase(it);
                continue;
            }
            const auto trade = it->second; // her answer below can end it

            // His request: a member of his club, or one of the world's who
            // qualifies as his recruit, accepts at once while a pearl could
            // change hands; wearing one already she declines, and so does one
            // of the world's short of the pearl's lock, or a stranger
            if (!trade.open)
            {
                if (!memberOf(PPlayer, charid).has_value() && !isRecruit(PPlayer, charid))
                {
                    trades.erase(charid);
                    declineTrade(PPawn, PPlayer, pawn::seats::isWorlds(charid) ? rules::Verdict::TooSoon : rules::Verdict::NotTrading);
                    continue;
                }
                auto* PWorn = wornItem(PPawn);
                if (const auto verdict = rules::judgeRequest(shellsHere(PPlayer, charid), PWorn != nullptr ? PWorn->GetLSID() : 0); verdict != rules::Verdict::Take)
                {
                    trades.erase(charid);
                    declineTrade(PPawn, PPlayer, verdict);
                    continue;
                }
                answerTrade(PPawn, GP_CLI_COMMAND_TRADE_RES_KIND::Start);
                if (const auto opened = trades.find(charid); opened != trades.end() && !opened->second.open)
                {
                    trades.erase(opened); // the game refused the window (out of reach)
                }
                continue;
            }
            if (!trade.confirmed)
            {
                continue;
            }

            // He pressed Trade: one Linkpearl of his shell and nothing else,
            // or she declines
            auto*      PWorn   = wornItem(PPawn);
            const auto verdict = rules::judgeOffer(trade.slots, shellsHere(PPlayer, charid), PWorn != nullptr ? PWorn->GetLSID() : 0);
            trades.erase(charid);
            if (verdict != rules::Verdict::Take)
            {
                declineTrade(PPawn, PPlayer, verdict);
                continue;
            }
            const auto lsid    = std::ranges::find_if(trade.slots, [](const rules::Offered& slot) { return slot.qty > 0; })->lsid;
            const bool joining = isRecruit(PPlayer, charid);
            answerTrade(PPawn, GP_CLI_COMMAND_TRADE_RES_KIND::Make);
            if (const auto slot = pearlIn(PPawn, lsid); slot.has_value())
            {
                putOn(PPlayer, PPawn, *slot, joining);
            }
            else
            {
                ShowInfoFmt("club: {}'s trade of a linkpearl to {} did not go through", PPlayer->getName(), PPawn->getName());
            }
        }
    }

    void tellHeadingYourWay(const CCharEntity* PPawn, const uint32 playerCharID)
    {
        auto* PPlayer = zoneutils::GetChar(playerCharID);
        if (PPawn == nullptr || PPlayer == nullptr)
        {
            return;
        }
        speak(PPlayer, PPawn, rules::headingYourWay(PPawn->id));
    }
} // namespace pawn::club
