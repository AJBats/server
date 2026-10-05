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

#include "offers.h"

#include "cardian_link.h"
#include "players.h"

#include "common/logging.h"
#include "entities/char_entity.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace pawn::offers
{
    namespace
    {
        using namespace cardian::link;

        std::unordered_map<uint8, Resolver>   resolvers; // by kind
        std::unordered_map<uint32, Offer>     open;      // by the player's charid
        uint32                                nextId = 0;
        timer::time_point                     lastLook{};
        constexpr auto                        kLookEvery = std::chrono::seconds(1);

        // His addon is told the question is down
        void tellGone(const Offer& offer)
        {
            auto msg  = make<cl_offer>();
            msg.offer = offer.id;
            msg.kind  = offer.kind;
            send(offer.player, msg, CL_S_OFFER_GONE);
        }

        void resolve(CCharEntity* PPlayer, const Offer& offer, const bool yes)
        {
            const auto it = resolvers.find(offer.kind);
            if (it == resolvers.end())
            {
                ShowErrorFmt("offer: nothing answers a question of kind {} ({}'s #{})", offer.kind, PPlayer->getName(), offer.id);
                return;
            }
            it->second(PPlayer, offer, yes);
        }
    } // namespace

    void setResolver(const uint8 kind, Resolver resolver)
    {
        resolvers[kind] = std::move(resolver);
    }

    auto put(CCharEntity* PPlayer, Offer offer, const std::chrono::seconds patience) -> bool
    {
        if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
        {
            return false;
        }
        if (const auto it = open.find(PPlayer->id); it != open.end())
        {
            ShowInfoFmt("offer: {}'s question #{} is withdrawn for a new one", PPlayer->getName(), it->second.id);
            tellGone(it->second);
            open.erase(it);
        }

        nextId         = nextId % 0x7FFFFFFF + 1;
        offer.id       = nextId;
        offer.player   = PPlayer->id;
        offer.zone     = static_cast<uint16>(PPlayer->getZone());
        offer.deadline = timer::now() + patience;

        auto msg    = make<cl_offer>();
        msg.offer   = offer.id;
        msg.kind    = offer.kind;
        msg.seconds = static_cast<uint16_t>(std::clamp<int64>(patience.count(), 0, UINT16_MAX));
        if (!send(PPlayer->id, msg))
        {
            return false;
        }
        ShowInfoFmt("offer: {} is asked #{} (kind {}), {} s to answer", PPlayer->getName(), offer.id, offer.kind, patience.count());
        open[PPlayer->id] = std::move(offer);
        return true;
    }

    auto answer(CCharEntity* PPlayer, const uint32 id, const bool yes) -> uint16
    {
        const auto it = PPlayer != nullptr ? open.find(PPlayer->id) : open.end();
        if (it == open.end() || it->second.id != id)
        {
            return CL_S_OFFER_GONE;
        }
        const Offer offer = std::move(it->second);
        open.erase(it);
        // Past its time or out of its zone, the question has lapsed even when
        // the once-a-second look has not caught it yet: a no, whatever he said
        if (timer::now() >= offer.deadline || static_cast<uint16>(PPlayer->getZone()) != offer.zone)
        {
            ShowInfoFmt("offer: {} answers #{} {} after it lapsed: a no", PPlayer->getName(), offer.id, yes ? "yes" : "no");
            resolve(PPlayer, offer, false);
            return CL_S_OFFER_GONE;
        }
        ShowInfoFmt("offer: {} answers #{} {}", PPlayer->getName(), offer.id, yes ? "yes" : "no");
        resolve(PPlayer, offer, yes);
        return CL_S_OK;
    }

    void tick()
    {
        const auto now = timer::now();
        if (open.empty() || now - lastLook < kLookEvery)
        {
            return;
        }
        lastLook = now;

        // Taken out of the table before any resolver runs: a resolver may
        // put a question of its own
        std::vector<Offer> gone;
        std::vector<Offer> lapsed;
        for (auto it = open.begin(); it != open.end();)
        {
            const Offer& offer = it->second;
            if (!players::online(offer.player))
            {
                gone.push_back(std::move(it->second));
                it = open.erase(it);
                continue;
            }
            // Between zones he has no body: his question waits until he lands
            const auto* PPlayer = zoneutils::GetChar(offer.player);
            if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
            {
                ++it;
                continue;
            }
            if (now >= offer.deadline || static_cast<uint16>(PPlayer->getZone()) != offer.zone)
            {
                lapsed.push_back(std::move(it->second));
                it = open.erase(it);
                continue;
            }
            ++it;
        }

        for (const auto& offer : gone)
        {
            ShowInfoFmt("offer: #{} goes with its player ({}), who has signed out", offer.id, offer.player);
        }
        for (const auto& offer : lapsed)
        {
            auto* PPlayer = zoneutils::GetChar(offer.player);
            if (PPlayer == nullptr)
            {
                continue;
            }
            const bool left = static_cast<uint16>(PPlayer->getZone()) != offer.zone;
            ShowInfoFmt("offer: {}'s #{} lapses ({}): a no", PPlayer->getName(), offer.id, left ? "he left the zone" : "no answer in time");
            tellGone(offer);
            resolve(PPlayer, offer, false);
        }
    }
} // namespace pawn::offers
