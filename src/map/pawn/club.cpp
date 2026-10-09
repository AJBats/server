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

#include "cardian_link.h"
#include "errands.h"
#include "party_finder.h"
#include "pawn.h"
#include "pawn_items.h"
#include "seats.h"
#include "world.h"

#include "common/database.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "enums/item_state.h"
#include "enums/chat_message_type.h"
#include "enums/party_kind.h"
#include "item_container.h"
#include "items/item_linkshell.h"
#include "items/transactions/item_claim.h"
#include "linkshell.h"
#include "packets/c2s/0x0c4_group_comlink_active.h"
#include "packets/s2c/0x017_chat_std.h"
#include "packets/s2c/0x0dc_group_solicit_req.h"
#include "party.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace pawn::club
{
    namespace
    {
        using namespace cardian::link;
        namespace rules = cardian::club;

        // Who wears which shell's pearl, off the items (read): by the wearer
        std::unordered_map<uint32, Pearl> worn;

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

        // One of the world's in his party, wearing no pearl: he can trade her one
        auto isGuest(const CCharEntity* PPlayer, const uint32 charid) -> bool
        {
            const auto* PPawn = pawn::findPawn(charid);
            return inPartyWith(PPawn, PPlayer) && pawn::world::hasBody(charid) && !worn.contains(charid);
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
                row.errand      = static_cast<uint8_t>(errand->kind);
                row.errandState = static_cast<uint8_t>(errand->state);
                row.secondsLeft = errand->secondsLeft;
                row.errandZone  = errand->zone;
                setText(row.errandZoneName, zoneName(errand->zone));
                setText(row.errandTitle, errand->title);
            }

            using cardian::errand::Kind;
            if (kind != Member::Guest)
            {
                if (errand.has_value())
                {
                    row.can |= CL_CAN_CALL_BACK;
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

        // His club, then the world's in his party who could be traded a
        // pearl; his shell, and with none the vendor of the city he stands in
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
            if (PChar->PParty != nullptr)
            {
                for (auto* PMember : PChar->PParty->members)
                {
                    if (PMember != nullptr && PMember != PChar && isGuest(PChar, PMember->id))
                    {
                        send(PMember->id, Member::Guest);
                    }
                }
            }
            reply.finish(answer, CL_S_OK);
        }

        // ---- the pearl ---------------------------------------------------------

        // A Linkpearl of his shell traded to her from his inventory, within
        // trading reach, and put on by her
        auto give(CCharEntity* PPlayer, const uint32 charid) -> uint16
        {
            const auto shells = shellsOf(PPlayer);
            if (shells.empty())
            {
                return CL_S_NO_LINKSHELL;
            }
            const auto member = memberOf(PPlayer, charid);
            const bool guest  = !member.has_value() && isGuest(PPlayer, charid);
            if (!member.has_value() && !guest)
            {
                return CL_S_NOT_IN_CLUB;
            }
            auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr || PPawn->loc.zone == nullptr)
            {
                return CL_S_NOT_STANDING;
            }
            if (auto* PWorn = wornItem(PPawn); PWorn != nullptr)
            {
                return shells.contains(PWorn->GetLSID()) ? CL_S_HAS_PEARL : CL_S_PEARLED_ELSEWHERE;
            }
            const auto slots = pearlToTrade(PPlayer, shells);
            if (slots.empty())
            {
                return CL_S_NO_PEARL_TO_GIVE;
            }
            uint8 landed = 0;
            if (const auto status = pawn::items::giveToPawn(PPlayer, PPawn, slots.front(), 1, &landed); status != CL_S_OK)
            {
                return status;
            }
            const auto name = PPawn->getName();
            if (!wear(PPawn, landed))
            {
                ShowErrorFmt("club: {} took {}'s linkpearl but the game would not let her put it on (slot {})", name, PPlayer->getName(), landed);
                say(PPlayer, fmt::format("{} takes the linkpearl.", name));
                read();
                return CL_S_OK;
            }
            read();
            // The recruit: one of the world's is held for him from now on,
            // where she stands, and stays wild
            if (guest)
            {
                pawn::world::keepFor(charid, PPlayer->id);
            }
            ShowInfoFmt("club: {} trades {} ({}) a linkpearl, and she puts it on{}", PPlayer->getName(), name, charid,
                        guest ? ": she joins his linkshell, wild as ever" : "");
            say(PPlayer, guest ? fmt::format("{} puts on the linkpearl and joins the linkshell.", name) : fmt::format("{} puts on the linkpearl.", name));
            return CL_S_OK;
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
            reply.finish(ask, ask.on != 0 ? give(PChar, ask.cardian) : breakPearl(PChar, ask.cardian));
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

    void tellHeadingYourWay(const CCharEntity* PPawn, const uint32 playerCharID)
    {
        auto* PPlayer = zoneutils::GetChar(playerCharID);
        if (PPawn == nullptr || PPlayer == nullptr)
        {
            return;
        }
        PPlayer->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPawn, MESSAGE_TELL, std::string(rules::headingYourWay(PPawn->id)));
    }
} // namespace pawn::club
