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

#include "party_roster.h"

#include "cardian_link_messages.h"
#include "pawn.h"
#include "pawn_spellbook.h"

#include "common/logging.h"
#include "entities/char_entity.h"
#include "items/item_equipment.h"
#include "party.h"
#include "spell.h"
#include "utils/zoneutils.h"

#include "common/timer.h"

#include <algorithm>
#include <optional>
#include <unordered_map>

namespace pawn::roster
{
    namespace
    {
        using cardian::party::Held;
        using cardian::party::Member;
        using cardian::party::Role;

        // player charid -> member charid -> the role he chose for her
        std::unordered_map<uint32, std::unordered_map<uint32, Role>> chosenByPlayer;

        // player charid -> his party as last settled, so a role that changes
        // is said once in the map log and not at every look
        std::unordered_map<uint32, std::vector<Held>> lastSettled;

        // player charid -> his party as last settled, kept two seconds for the
        // engines, which ask every tick; the screen settles afresh
        struct Settled
        {
            timer::time_point at{};
            std::vector<Held> held;
        };
        std::unordered_map<uint32, Settled> settledFor;

        // member charid -> her role as last read, for the tick that cannot
        // reach her player: between zones, hers or his, the party is unchanged
        // and the role holds for a while rather than coming off and on again
        struct LastRole
        {
            timer::time_point at{};
            Role              role = Role::None;
        };
        std::unordered_map<uint32, LastRole> lastRoleOf;
        constexpr auto                       kRoleGrace = std::chrono::seconds(60);

        // His party's characters: himself first, then the others in the
        // order the party holds them. Alone, he is his whole party
        auto membersOf(CCharEntity* PPlayer) -> std::vector<CCharEntity*>
        {
            std::vector<CCharEntity*> out{ PPlayer };
            if (PPlayer->PParty != nullptr)
            {
                for (auto* PMember : PPlayer->PParty->members)
                {
                    if (auto* PChar = dynamic_cast<CCharEntity*>(PMember); PChar != nullptr && PChar != PPlayer)
                    {
                        out.push_back(PChar);
                    }
                }
            }
            return out;
        }

        auto holds(const std::vector<CCharEntity*>& members, const uint32 memberId) -> bool
        {
            return std::ranges::any_of(members, [memberId](const CCharEntity* PChar)
                                       {
                                           return PChar->id == memberId;
                                       });
        }

        // What the join rule reads of her, as she is now, with the player's
        // choice for her when he has made one
        auto factsOf(CCharEntity* PChar, const std::unordered_map<uint32, Role>* chosen) -> Member
        {
            Member m;
            m.id  = PChar->id;
            m.job = PChar->GetMJob();
            if (const auto* POffHand = PChar->getEquip(SLOT_SUB); POffHand != nullptr)
            {
                m.shield = POffHand->IsShield();
            }
            m.castsNi   = CSpellBook::Eligible(PChar, spell::GetSpell(SpellID::Utsusemi_Ni));
            m.castsIchi = CSpellBook::Eligible(PChar, spell::GetSpell(SpellID::Utsusemi_Ichi));
            if (chosen != nullptr)
            {
                if (const auto it = chosen->find(PChar->id); it != chosen->end())
                {
                    m.chosen = it->second;
                }
            }
            return m;
        }

        auto factsOf(CCharEntity* PPlayer, const std::vector<CCharEntity*>& members) -> std::vector<Member>
        {
            const auto  it     = chosenByPlayer.find(PPlayer->id);
            const auto* chosen = it != chosenByPlayer.end() ? &it->second : nullptr;

            std::vector<Member> facts;
            facts.reserve(members.size());
            for (auto* PChar : members)
            {
                facts.push_back(factsOf(PChar, chosen));
            }
            return facts;
        }

        // Each role that differs from the last settle of this player's
        // party, once: a member new to it, a role the rule moved, a choice
        void sayChanges(const CCharEntity* PPlayer, const std::vector<CCharEntity*>& members, const std::vector<Held>& held)
        {
            auto& last = lastSettled[PPlayer->id];
            for (std::size_t i = 0; i < held.size(); ++i)
            {
                const auto was = std::ranges::find_if(last, [&](const Held& h)
                                                      {
                                                          return h.id == held[i].id;
                                                      });
                if (was != last.end() && was->role == held[i].role && was->byPlayer == held[i].byPlayer)
                {
                    continue;
                }
                ShowInfoFmt("roles: {} is {} in {}'s party ({})", members[i]->getName(), cardian::party::roleName(held[i].role),
                            PPlayer->getName(), held[i].byPlayer ? "his choice" : "the join rule");
            }
            last = held;
        }

        // His party settled: afresh, or as kept within the last two seconds.
        // One settle for the screen and the engines, so the two never disagree
        auto settleParty(CCharEntity* PPlayer, const bool fresh) -> const std::vector<Held>&
        {
            auto&      settled = settledFor[PPlayer->id];
            const auto now     = timer::now();
            if (fresh || settled.held.empty() || now - settled.at >= std::chrono::seconds(2))
            {
                const auto members = membersOf(PPlayer);
                settled.held       = cardian::party::settle(factsOf(PPlayer, members));
                settled.at         = now;
                sayChanges(PPlayer, members, settled.held);
            }
            return settled.held;
        }
    } // namespace

    auto rolesOf(CCharEntity* PPlayer) -> std::vector<Row>
    {
        std::vector<Row> rows;
        if (PPlayer == nullptr)
        {
            return rows;
        }
        // Settled afresh, in the party's order, which membersOf gives again
        const auto& held    = settleParty(PPlayer, true);
        const auto  members = membersOf(PPlayer);
        rows.reserve(members.size());
        for (std::size_t i = 0; i < members.size() && i < held.size(); ++i)
        {
            auto* PChar = members[i];
            Row   row;
            row.who       = PChar;
            row.id        = PChar->id;
            row.name      = PChar->getName();
            row.mainJob   = static_cast<uint8>(PChar->GetMJob());
            row.mainLevel = PChar->GetMLevel();
            row.subJob    = static_cast<uint8>(PChar->GetSJob());
            row.subLevel  = PChar->GetSLevel();
            row.role      = held[i].role;
            row.byPlayer  = held[i].byPlayer;
            rows.push_back(std::move(row));
        }
        return rows;
    }

    auto roleOf(CCharEntity* PMember) -> Role
    {
        if (PMember == nullptr)
        {
            return Role::None;
        }
        // The real player in her party: the orders' owner (ordersOwnerOf).
        // Out of his party (an alt standing by) she holds no role. Between
        // zones, his (not to be found) or hers (off the party's list for the
        // moment), the party is unchanged: her last role holds for a while
        // (kRoleGrace), rather than the role layer coming off and on again
        // around every zone line. Leaving the party forgets it (memberLeft)
        auto&      last    = lastRoleOf[PMember->id];
        const auto now     = timer::now();
        const auto owner   = pawn::ordersOwnerOf(PMember);
        auto*      PPlayer = owner != 0 ? zoneutils::GetChar(owner) : nullptr;
        const bool withHim = PPlayer != nullptr && (PPlayer == PMember || (PMember->PParty != nullptr && PMember->PParty == PPlayer->PParty));

        std::optional<Role> role;
        if (withHim)
        {
            for (const auto& h : settleParty(PPlayer, false))
            {
                if (h.id == PMember->id)
                {
                    role = h.role;
                    break;
                }
            }
        }
        if (!role.has_value())
        {
            const bool recent = last.at != timer::time_point{} && now - last.at < kRoleGrace;
            return recent ? last.role : Role::None;
        }
        last = { now, *role };
        return *role;
    }

    auto choose(CCharEntity* PPlayer, const uint32 memberId, const Role role) -> uint16
    {
        if (PPlayer == nullptr)
        {
            return CL_S_NOT_IN_PARTY;
        }
        const auto members = membersOf(PPlayer);
        if (!holds(members, memberId))
        {
            return CL_S_NOT_IN_PARTY;
        }
        // The rule's own bookkeeping decides who else the choice touches (a
        // role only one member holds is taken from his earlier choice)
        auto facts = factsOf(PPlayer, members);
        cardian::party::choose(facts, memberId, role);

        settledFor.erase(PPlayer->id);
        auto& chosen = chosenByPlayer[PPlayer->id];
        for (const auto& m : facts)
        {
            if (m.chosen.has_value())
            {
                chosen[m.id] = *m.chosen;
            }
        }
        // A member across a zone line is off the party's list for a moment, and
        // so not among the facts: his earlier choice of this role for her gives
        // way all the same, or two members would hold it by his choice
        if (cardian::party::heldByOne(role))
        {
            for (auto& [id, pick] : chosen)
            {
                if (id != memberId && pick == role)
                {
                    pick = Role::None;
                }
            }
        }
        return CL_S_OK;
    }

    auto release(CCharEntity* PPlayer, const uint32 memberId) -> uint16
    {
        if (PPlayer == nullptr || !holds(membersOf(PPlayer), memberId))
        {
            return CL_S_NOT_IN_PARTY;
        }
        if (const auto it = chosenByPlayer.find(PPlayer->id); it != chosenByPlayer.end())
        {
            it->second.erase(memberId);
        }
        settledFor.erase(PPlayer->id);
        return CL_S_OK;
    }

    void memberLeft(const CBattleEntity* PMember)
    {
        if (PMember == nullptr)
        {
            return;
        }
        chosenByPlayer.erase(PMember->id);
        lastSettled.erase(PMember->id);
        lastRoleOf.erase(PMember->id);
        // Whichever party she left is settled afresh at its next read
        settledFor.clear();
        for (auto& [player, chosen] : chosenByPlayer)
        {
            chosen.erase(PMember->id);
        }
    }
} // namespace pawn::roster
