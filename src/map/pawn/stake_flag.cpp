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

#include "stake_flag.h"

#include "common/logging.h"

#include "entities/char_entity.h"
#include "entities/npc_entity.h"
#include "instance.h"
#include "lua/luautils.h"
#include "packets/basic.h"
#include "utils/zoneutils.h"
#include "zone.h"
#include "zone_instance.h"

#include <unordered_map>

namespace cardian::stakeflag
{
    namespace
    {
        // The conquest banners: four flags on poles the client already carries,
        // and -- the thing that makes this workable -- the SAME model ids in
        // every zone that has them (817 alone appears in 32 zones, from East
        // Ronfaure to Xarcabard), so a flag chosen once flies anywhere.
        //
        //   814 San d'Oria   815 Bastok   816 Windurst   817 the beastmen
        //
        // The game stands all four on one spot and shows whichever nation holds
        // the region; we stand exactly one.
        constexpr uint16 kSandoriaBanner = 814;
        constexpr uint16 kBeastmenBanner = 817;

        // Owner charid -> the flag's entity id. A dynamic entity is written to
        // no table and survives no restart, so this map is the only handle we
        // have on a banner once it stands.
        std::unordered_map<uint32, EntityId> flagByOwner;

        // A player of no nation (Jeuno and above) flies the beastmen's, which is
        // at least a flag. What he *should* fly is a design question the trial
        // is not trying to answer.
        auto bannerFor(const CCharEntity* POwner) -> uint16
        {
            return POwner->profile.nation <= 2
                       ? static_cast<uint16>(kSandoriaBanner + POwner->profile.nation)
                       : kBeastmenBanner;
        }
    } // namespace

    void plant(CCharEntity* POwner, const position_t& at)
    {
        if (POwner == nullptr || POwner->loc.zone == nullptr)
        {
            return;
        }

        // A stake that moved takes its flag with it
        dissolve(POwner->id);

        if (dynamic_cast<CZoneInstance*>(POwner->loc.zone) != nullptr && POwner->PInstance == nullptr)
        {
            ShowWarningFmt("stake flag: {} has no instance for the banner", POwner->getName());
            return;
        }
        const auto* entities = POwner->PInstance != nullptr ? static_cast<const CZoneEntities*>(POwner->PInstance) : POwner->loc.zone->GetZoneEntities();
        // Dynamic IDs span 0x700..0x8ff; upstream allocation logs exhaustion
        // but still returns an invalid 0x900 entity instead of nullptr.
        if (entities->GetUsedDynamicTargIDsCount() >= 0x200)
        {
            ShowWarningFmt("stake flag: no dynamic IDs left for {}'s banner", POwner->getName());
            return;
        }

        const uint16 banner = bannerFor(POwner);

        // A decoration, made the way the game's own seasonal props are made
        // (scripts/events/egg_hunt_egg-stravaganza.lua): nameless, off widescan,
        // never targetable, and its targid released the moment it disappears.
        auto* PFlag = luautils::GenerateDynamicEntity(POwner->loc.zone, POwner->PInstance,
                                                      lua.create_table_with(
                                                          "name", "     ",
                                                          "look", banner,
                                                          "x", at.x,
                                                          "y", at.y,
                                                          "z", at.z,
                                                          "rotation", at.rotation,
                                                          "widescan", 0,
                                                          "entityFlags", 2075,
                                                          "namevis", 64,
                                                          "releaseIdOnDisappear", true));
        if (PFlag == nullptr)
        {
            ShowWarningFmt("stake flag: the zone refused {}'s banner (model {})", POwner->getName(), banner);
            return;
        }

        flagByOwner[POwner->id] = EntityId(PFlag);
        ShowInfoFmt("stake flag: {}'s banner (model {}) stands on the stake at ({:.1f}, {:.1f}, {:.1f}), facing {} deg",
                    POwner->getName(), banner, at.x, at.y, at.z, at.rotation * 360 / 256);
    }

    void dissolve(const uint32 ownerCharID)
    {
        const auto it = flagByOwner.find(ownerCharID);
        if (it == flagByOwner.end())
        {
            return;
        }

        // Tell the clients first, THEN let the zone reap it. The order matters:
        // the zone's reap (CZoneEntities::npcTick) erases a disappeared dynamic
        // NPC from every client's spawn list WITHOUT sending a despawn, on the
        // assumption that whatever set the status already told them. A flag put
        // down the other way round stands on screen forever -- the server has
        // forgotten it and the client has never been told.
        //
        // A flag whose zone has since unloaded is simply not found, and
        // forgetting it here is the right end for it.
        if (auto* PFlag = it->second.resolve<CNpcEntity>(); PFlag != nullptr)
        {
            if (PFlag->loc.zone != nullptr)
            {
                PFlag->loc.zone->UpdateEntityPacket(PFlag, ENTITY_DESPAWN, UPDATE_NONE);
            }
            PFlag->status = xi::Status::Disappear;
        }

        flagByOwner.erase(it);
    }

} // namespace cardian::stakeflag
