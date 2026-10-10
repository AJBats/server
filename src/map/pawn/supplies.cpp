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

#include "supplies.h"

#include "cardian_link.h"
#include "gate_guards.h"
#include "link_api.h"
#include "pawn.h"

#include "ai/ai_container.h"
#include "ai/states/item_state.h"
#include "ai/states/magic_state.h"
#include "common/logging.h"
#include "common/timer.h"
#include "common/xirand.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "lua/lua_base_entity.h"
#include "party.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/itemutils.h"
#include "zone.h"

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::supplies
{
    namespace
    {
        using namespace std::chrono_literals;

        constexpr uint16 kInstantWarp    = 4181;
        constexpr uint16 kInstantReraise = 4182;

        // The scrolls she keeps, and the bit each is in SUPPLIES' missing
        constexpr std::array<std::pair<uint16, uint8>, 2> kScrolls{ { { kInstantWarp, 1 }, { kInstantReraise, 2 } } };

        constexpr float kBeside      = 12.0f;                   // yalms from him: she is at the guard with him
        constexpr auto  kSpeakAgain  = std::chrono::minutes(3); // a guard that would not sell to her, said again
        constexpr auto  kCaughtUp    = 8s;                      // after he arrives in a city: the party has come in behind him
        constexpr auto  kLookEvery   = 2s;

        // His arrival in a zone, and whether a cardian has spoken of the shop there
        struct Arrival
        {
            uint16            zone   = 0;
            timer::time_point at{};
            bool              spoken = false;
        };

        std::unordered_map<uint32, Arrival>                                  arrivals; // by his charid
        std::map<std::pair<uint32, std::string>, timer::time_point>          refused;  // (her charid, the guard) -> when he last would not sell
        std::unordered_map<uint16, timer::time_point>                        looked;   // by zone

        auto pointsOf(CCharEntity* PChar) -> int32
        {
            return charutils::GetPoints(PChar, charutils::GetConquestPointsName(PChar).c_str());
        }

        // A scroll in the guard's stock: its option and its price in points,
        // from the guard's own table (xi.cardian.exchange.common)
        struct Stock
        {
            uint32 option = 0;
            uint32 price  = 0;
        };
        auto stockOf(const uint16 itemId) -> std::optional<Stock>
        {
            static std::unordered_map<uint16, std::optional<Stock>> known;
            if (const auto it = known.find(itemId); it != known.end())
            {
                return it->second;
            }
            auto call = linkapi::libraryCall("exchange", "common");
            if (!call)
            {
                return std::nullopt; // its library did not load: asked again next time
            }
            const auto res   = (*call)(itemId);
            const auto entry = linkapi::libraryTable("exchange.common", res);
            if (!res.valid())
            {
                return std::nullopt; // the call failed (said once): asked again next time
            }
            std::optional<Stock> stock;
            if (entry)
            {
                stock = Stock{ entry->get_or<uint32>("option", 0), entry->get_or<uint32>("price", 0) };
            }
            if (!stock || stock->option == 0)
            {
                ShowErrorFmt("supplies: item {} is not in the conquest guards' common stock", itemId);
                stock.reset();
            }
            return known[itemId] = stock;
        }

        // The scrolls she lacks, by SUPPLIES' bits
        auto lacking(CCharEntity* PPawn) -> uint8
        {
            uint8 bits = 0;
            for (const auto& [itemId, bit] : kScrolls)
            {
                if (!charutils::HasItem(PPawn, itemId))
                {
                    bits |= bit;
                }
            }
            return bits;
        }

        // Her inventory has a slot free for a scroll she buys
        auto hasRoom(CCharEntity* PPawn) -> bool
        {
            const auto* storage = PPawn->getStorage(LOC_INVENTORY);
            return storage != nullptr && storage->GetFreeSlotsCount() > 0;
        }

        // She could pay for one of the scrolls she lacks, and carry it
        auto canPay(CCharEntity* PPawn, const uint8 lacks) -> bool
        {
            if (!hasRoom(PPawn))
            {
                return false;
            }
            const int32 points = pointsOf(PPawn);
            for (const auto& [itemId, bit] : kScrolls)
            {
                if ((lacks & bit) != 0)
                {
                    if (const auto stock = stockOf(itemId); stock && points >= static_cast<int32>(stock->price))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        // The cardians of his party in his zone, alive: within `reach` of him
        // when given
        auto partyWith(CCharEntity* PPlayer, const std::optional<float> reach) -> std::vector<CCharEntity*>
        {
            std::vector<CCharEntity*> out;
            for (auto* PMember : PPlayer->PParty->members)
            {
                auto* PPawn = dynamic_cast<CCharEntity*>(PMember);
                if (PPawn == nullptr || PPawn == PPlayer || !isPawn(PPawn) || PPawn->loc.zone != PPlayer->loc.zone || PPawn->isDead() ||
                    (reach && distance(PPawn->loc.p, PPlayer->loc.p) > *reach))
                {
                    continue;
                }
                out.push_back(PPawn);
            }
            return out;
        }

        void tell(const CCharEntity* PPlayer, const CCharEntity* PPawn, const uint8 kind, const uint8 missing = 0, const uint16 item = 0, const uint16 price = 0)
        {
            auto msg    = cardian::link::make<cl_supplies>();
            msg.cardian = PPawn->id;
            msg.kind    = kind;
            msg.nation  = PPawn->profile.nation;
            msg.missing = missing;
            msg.item    = item;
            msg.price   = price;
            cardian::link::send(PPlayer->id, msg);
        }

        // Warping out: reading or casting her way home, or under way --
        // nobody shops on her way out of town
        auto leaving(const CCharEntity* PChar) -> bool
        {
            return PChar->requestedWarp != WarpRequest::None || PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Teleport) ||
                   PChar->PAI->IsCurrentState<CItemState>() || PChar->PAI->IsCurrentState<CMagicState>();
        }

        // What she can buy of what she lacks, through the guard's own sale:
        // each scroll she lacks and can pay for, while her bag has room. The
        // player told of each purchase, and of a foreign guard's refusal,
        // when given. How many she bought, and whether the guard refused her
        struct Bought
        {
            uint32      count   = 0;
            bool        refused = false;
            std::string refusal; // the guard's reason, in the exchange's terms
        };
        auto purchase(CCharEntity* PPawn, const guards::Guard& guard, const CCharEntity* PTell) -> Bought
        {
            Bought      out;
            const uint8 lacks  = lacking(PPawn);
            int32       points = pointsOf(PPawn);
            for (const auto& [itemId, bit] : kScrolls)
            {
                const auto stock = (lacks & bit) != 0 ? stockOf(itemId) : std::nullopt;
                if (!stock || points < static_cast<int32>(stock->price))
                {
                    continue; // has it, or cannot pay: nothing to ask
                }
                if (!hasRoom(PPawn))
                {
                    return out; // her bag is full: nothing she buys would fit
                }
                auto call = linkapi::libraryCall("exchange", "buy");
                auto sale = call ? linkapi::libraryTable("exchange.buy", (*call)(CLuaBaseEntity(PPawn), guard.nation, guard.type, stock->option)) : std::nullopt;
                if (!sale)
                {
                    out.refused = true; // the sale failed (said by the call)
                    out.refusal = "FAILED";
                    return out;
                }
                const auto* PItem = xi::items::lookup(itemId);
                const auto  name  = PItem != nullptr ? PItem->getName() : std::to_string(itemId);
                if (const auto refusal = sale->get<sol::optional<std::string>>("refusal"))
                {
                    out.refused = true;
                    out.refusal = *refusal;
                    ShowInfoFmt("supplies: {} cannot buy {} from {} ({})", PPawn->getName(), name, guard.name, *refusal);
                    if (PTell != nullptr && (*refusal == "OUTRANKED" || *refusal == "FOREIGN_PLACE"))
                    {
                        tell(PTell, PPawn, CL_SUPPLIES_FOREIGN);
                    }
                    return out;
                }
                points = static_cast<int32>(sale->get_or<uint32>("cp", 0));
                ++out.count;
                ShowInfoFmt("supplies: {} buys {} from {} for {} conquest points ({} left)", PPawn->getName(), name, guard.name, stock->price, points);
                if (PTell != nullptr)
                {
                    tell(PTell, PPawn, CL_SUPPLIES_BOUGHT, 0, itemId, static_cast<uint16>(stock->price));
                }
            }
            return out;
        }

        // She buys what she lacks from the guard he stands by, through the
        // guard's own sale; a guard who will not sell to her is left alone
        // for kSpeakAgain, and his refusal said if it is her nation's
        void shopAt(CCharEntity* PPlayer, const guards::Guard& guard, CCharEntity* PPawn, const timer::time_point now)
        {
            if (leaving(PPlayer) || leaving(PPawn))
            {
                return;
            }
            const auto key = std::make_pair(PPawn->id, std::string(guard.name));
            if (const auto it = refused.find(key); it != refused.end() && now - it->second < kSpeakAgain)
            {
                return;
            }
            if (purchase(PPawn, guard, PPlayer).refused)
            {
                refused[key] = now; // not asked again at this guard for kSpeakAgain
            }
        }

        // His arrival in a city, once the party has come in behind him: one
        // of his cardians there who lacks a scroll and could pay for it says
        // she wants the conquest shop
        void arrivedIn(CCharEntity* PPlayer, const timer::time_point now)
        {
            // his zone-in is the arrival (zonedIn); a zone he is found in
            // without one -- the map started under him -- is one too
            auto& arrival = arrivals[PPlayer->id];
            if (arrival.zone != static_cast<uint16>(PPlayer->getZone()))
            {
                arrival = Arrival{ static_cast<uint16>(PPlayer->getZone()), now, false };
            }
            // a city with a conquest shop: Jeuno's, the nations', not Selbina's or Mhaura's
            const bool city = (PPlayer->loc.zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown &&
                              guards::zoneHasGuard(PPlayer->loc.zone->getName());
            if (!city || arrival.spoken || now - arrival.at < kCaughtUp || PPlayer->inMogHouse())
            {
                return;
            }
            arrival.spoken = true;
            std::vector<std::pair<CCharEntity*, uint8>> wanting;
            for (auto* PPawn : partyWith(PPlayer, std::nullopt))
            {
                if (const uint8 lacks = lacking(PPawn); lacks != 0 && canPay(PPawn, lacks))
                {
                    wanting.emplace_back(PPawn, lacks);
                }
            }
            if (wanting.empty())
            {
                return;
            }
            const auto& [PPawn, lacks] = wanting[xirand::GetRandomNumber(wanting.size())];
            ShowInfoFmt("supplies: {} wants the conquest shop in {} (lacks {})", PPawn->getName(), PPlayer->loc.zone->getName(), lacks);
            tell(PPlayer, PPawn, CL_SUPPLIES_WANTED, lacks);
        }
    } // namespace

    auto buyAt(CCharEntity* PPawn, const guards::Guard& guard, const CCharEntity* PTell, std::string* refusal) -> uint32
    {
        if (PPawn == nullptr || leaving(PPawn))
        {
            return 0;
        }
        const auto bought = purchase(PPawn, guard, PTell);
        if (refusal != nullptr)
        {
            *refusal = bought.refusal;
        }
        return bought.count;
    }

    void zonedIn(const CCharEntity* PPlayer)
    {
        if (PPlayer != nullptr && !isPawn(PPlayer) && PPlayer->loc.zone != nullptr)
        {
            arrivals[PPlayer->id] = Arrival{ static_cast<uint16>(PPlayer->getZone()), timer::now(), false };
        }
    }

    void tick(CZone* PZone)
    {
        const auto now = timer::now();
        auto&      at  = looked[static_cast<uint16>(PZone->GetID())];
        if (now - at < kLookEvery)
        {
            return;
        }
        at = now;

        PZone->ForEachChar([&](CCharEntity* PChar)
        {
            if (PChar == nullptr || isPawn(PChar) || PChar->PParty == nullptr || PChar->loc.zone == nullptr)
            {
                return;
            }
            arrivedIn(PChar, now);
            if (const auto* PGuard = guards::guardNear(PChar); PGuard != nullptr)
            {
                for (auto* PPawn : partyWith(PChar, kBeside))
                {
                    shopAt(PChar, *PGuard, PPawn, now);
                }
            }
        });
    }
} // namespace pawn::supplies
