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

#include "riding.h"

#include "errands.h"
#include "link_api.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "riding_math.h"
#include "town_errands.h"
#include "world.h"

#include "ai/ai_container.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/lua.h"
#include "common/settings.h"
#include "common/utils.h"
#include "entities/char_entity.h"
#include "lua/lua_base_entity.h"
#include "party.h"
#include "status_effect_container.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"

#include <array>
#include <optional>

namespace pawn::riding
{
    namespace
    {
        // The license's key item, by its name in the game's own table
        // (xi.keyItem), read once Lua has it
        auto licenseItem() -> std::optional<xi::KeyItem>
        {
            static std::optional<xi::KeyItem> item;
            if (!item.has_value())
            {
                if (const auto id = ::lua["xi"]["keyItem"]["CHOCOBO_LICENSE"].get<sol::optional<uint16>>(); id.has_value())
                {
                    item = static_cast<xi::KeyItem>(*id);
                }
            }
            return item;
        }
    } // namespace

    auto licensed(CCharEntity* PChar) -> bool
    {
        const auto item = licenseItem();
        if (PChar == nullptr || !item.has_value())
        {
            return false;
        }
        if (charutils::hasKeyItem(PChar, *item))
        {
            return true;
        }
        const auto grant = [&](const std::string_view why)
        {
            charutils::addKeyItem(PChar, *item);
            charutils::SaveKeyItems(PChar);
            ShowInfoFmt("riding: {} holds the Chocobo License now ({})", PChar->getName(), why);
            return true;
        };
        // A cardian his account owns holds it when he does (an owned cardian
        // has an account of her own, which account_wide.cpp never looks at)
        if (!pawn::world::hasBody(PChar->id))
        {
            const auto* PPlayer = pawn::partyPlayer(PChar);
            if (PPlayer == nullptr || !charutils::hasKeyItem(PPlayer, *item))
            {
                return false;
            }
            const auto rset = db::preparedStmt("SELECT owner_accid FROM cardian_pawns WHERE pawn_charid = ?", PChar->id);
            if (!rset || !rset->next() || rset->get<uint32>("owner_accid") != PPlayer->accid)
            {
                return false;
            }
            return grant(fmt::format("{}'s, through his account", PPlayer->getName()));
        }
        std::array<uint8, MAX_JOBTYPE> levels{};
        for (std::size_t job = 1; job < levels.size(); ++job)
        {
            levels[job] = PChar->jobs.job[job];
        }
        if (!cardian::riding::licensedByJobs(levels))
        {
            return false;
        }
        return grant(fmt::format("a job at {} or above", cardian::riding::kLicenseLevel));
    }

    auto licensedFor(const CCharEntity* PPlayer, const uint32 charid) -> bool
    {
        const CCharEntity* PChar = pawn::findPawn(charid);
        PChar                    = PChar != nullptr ? PChar : zoneutils::GetChar(charid);
        // a body of the world, by the census, holds it by her own jobs alone
        if (pawn::errands::ofTheWorld(charid))
        {
            uint8 top = 0;
            if (PChar != nullptr)
            {
                for (std::size_t job = 1; job < MAX_JOBTYPE; ++job)
                {
                    top = std::max(top, PChar->jobs.job[job]);
                }
            }
            else if (const auto rset = db::preparedStmt("SELECT GREATEST(war, mnk, whm, blm, rdm, thf, pld, drk, bst, brd, rng, sam, nin, drg, smn, blu, cor, pup, "
                                                        "dnc, sch, geo, run) AS top FROM char_jobs WHERE charid = ?",
                                                        charid);
                     rset && rset->next())
            {
                top = rset->get<uint8>("top");
            }
            return top >= cardian::riding::kLicenseLevel;
        }
        // anyone else by the account's license alone: the key item, his or hers
        const auto item = licenseItem();
        if (!item.has_value())
        {
            return false;
        }
        if (PPlayer != nullptr && charutils::hasKeyItem(PPlayer, *item))
        {
            return true;
        }
        if (PChar != nullptr)
        {
            return charutils::hasKeyItem(PChar, *item);
        }
        const auto rset = db::preparedStmt("SELECT keyitems FROM chars WHERE charid = ?", charid);
        if (!rset || !rset->next() || rset->isNull("keyitems"))
        {
            return false;
        }
        keyitems_t keys{};
        db::extractFromBlob(rset, "keyitems", keys);
        const auto id = static_cast<uint16>(*item);
        return (id >> 9) < keys.tables.size() && keys.tables[id >> 9].keyList.test(id & 511);
    }

    auto ridesFree(const CCharEntity* PChar) -> bool
    {
        return PChar != nullptr && pawn::world::hasBody(PChar->id);
    }

    auto ridersBeside(const CCharEntity* PPlayer) -> std::vector<Rider>
    {
        std::vector<Rider> riders;
        if (PPlayer == nullptr || PPlayer->PParty == nullptr || PPlayer->loc.zone == nullptr)
        {
            return riders;
        }
        for (auto* PMember : PPlayer->PParty->members)
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PMember);
            if (PChar == nullptr || PChar == PPlayer || !pawn::isPawn(PChar) || PChar->loc.zone != PPlayer->loc.zone || PChar->isDead() ||
                PChar->isMounted() || PChar->PAI->IsEngaged() || pawn::town::busy(PChar->id) ||
                distance(PChar->loc.p, PPlayer->loc.p) > cardian::riding::kBesideYalms)
            {
                continue;
            }
            const auto* PController = dynamic_cast<const CPawnController*>(PChar->PAI->GetController());
            if (PController != nullptr && (PController->IsWaiting() || PController->InManeuver()))
            {
                continue;
            }
            if (!licensed(PChar))
            {
                ShowInfoFmt("riding: {} stays behind at the stable: no Chocobo License", PChar->getName());
                continue;
            }
            riders.push_back({ PChar->id, ridesFree(PChar) });
        }
        return riders;
    }

    auto runSpeed(const CCharEntity* PPawn) -> uint8
    {
        return PPawn != nullptr && PPawn->isMounted() ? settings::get<uint8>("pawn.RIDE_SPEED") : settings::get<uint8>("pawn.PAWN_SPEED");
    }

    auto rent(CCharEntity* PPawn, CCharEntity* PPayer) -> bool
    {
        if (PPawn == nullptr || PPawn->loc.zone == nullptr)
        {
            return false;
        }
        auto call = pawn::linkapi::libraryCall("chocobo", "rent");
        if (!call)
        {
            return false; // its library did not load (said once)
        }
        const bool free = ridesFree(PPawn);
        const auto res  = PPayer != nullptr ? (*call)(CLuaBaseEntity(PPawn), CLuaBaseEntity(PPayer), free) : (*call)(CLuaBaseEntity(PPawn), sol::lua_nil, free);
        const auto out  = pawn::linkapi::libraryTable("chocobo.rent", res);
        if (!out)
        {
            return false; // the call failed (said by it)
        }
        if (const auto refusal = out->get<sol::optional<std::string>>("refusal"))
        {
            ShowInfoFmt("riding: {} rents no chocobo in {} ({})", PPawn->getName(), PPawn->loc.zone->getName(), *refusal);
            return false;
        }
        ShowInfoFmt("riding: {} rents a chocobo in {}{}", PPawn->getName(), PPawn->loc.zone->getName(), free ? ", free (the world's own)" : "");
        return true;
    }

    void dismount(CCharEntity* PPawn, const std::string_view why)
    {
        if (PPawn == nullptr || !PPawn->isMounted())
        {
            return;
        }
        PPawn->StatusEffectContainer->DelStatusEffect(xi::StatusEffect::Mounted);
        ShowInfoFmt("riding: {} gets down from her chocobo ({})", PPawn->getName(), why);
    }
} // namespace pawn::riding
