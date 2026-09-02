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

#include "cardian_link.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_gambits.h"
#include "pawn_items.h"

#include "common/logging.h"

#include "ai/ai_container.h"
#include "entities/char_entity.h"
#include "enums/packet_c2s.h"
#include "enums/packet_s2c.h"
#include "item_container.h"
#include "enums/party_kind.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "packets/basic.h"
#include "utils/moduleutils.h"

#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr auto kBrainScript = "./modules/cardian/lua/pawn/brain.lua";

    using namespace gambits;

    auto readPredicate(const sol::table& entry) -> Predicate_t
    {
        return { static_cast<G_CONDITION>(entry.get_or<uint16>(1, 0)), entry.get_or<uint32>(2, 0) };
    }

    auto readAction(const sol::table& entry) -> Action_t
    {
        return { static_cast<G_REACTION>(entry.get_or<uint16>(1, 0)), static_cast<G_SELECT>(entry.get_or<uint16>(2, 0)), entry.get_or<uint32>(3, 0) };
    }

    auto readLogicGroup(const sol::table& entry) -> PredicateGroup_t
    {
        std::vector<Predicate_t> predicates;
        if (const sol::optional<sol::table> nested = entry["conditions"]; nested.has_value())
        {
            for (const auto& pair : *nested)
            {
                if (pair.second.get_type() == sol::type::table)
                {
                    predicates.emplace_back(readPredicate(pair.second.as<sol::table>()));
                }
            }
        }
        return { static_cast<G_LOGIC>(entry.get_or<uint16>("logic", 0)), std::move(predicates) };
    }

    // The trust addGambit shapes, plus a bare ai.l.OR(...) as the whole
    // conditions argument:
    //   { condition, arg }
    //   { { condition, arg }, { condition, arg }, ai.l.OR({ c, a }, { c, a }) }
    //   ai.l.OR({ c, a }, { c, a })
    auto parsePredicates(const sol::table& conditions) -> std::vector<PredicateGroup_t>
    {
        std::vector<PredicateGroup_t> groups;

        if (conditions["logic"].valid())
        {
            groups.emplace_back(readLogicGroup(conditions));
        }
        else if (conditions[1].get_type() == sol::type::table)
        {
            for (const auto& pair : conditions)
            {
                if (pair.second.get_type() != sol::type::table)
                {
                    continue;
                }

                const sol::table entry = pair.second.as<sol::table>();
                if (entry["logic"].valid())
                {
                    groups.emplace_back(readLogicGroup(entry));
                }
                else
                {
                    groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ readPredicate(entry) });
                }
            }
        }
        else if (conditions[1].get_type() == sol::type::number)
        {
            groups.emplace_back(G_LOGIC::AND, std::vector<Predicate_t>{ readPredicate(conditions) });
        }

        return groups;
    }

    //   { reaction, select, arg }
    //   { { reaction, select, arg }, { reaction, select, arg } }
    auto parseActions(const sol::table& reactions) -> std::vector<Action_t>
    {
        std::vector<Action_t> actions;

        if (reactions[1].get_type() == sol::type::table)
        {
            for (const auto& pair : reactions)
            {
                if (pair.second.get_type() == sol::type::table)
                {
                    actions.emplace_back(readAction(pair.second.as<sol::table>()));
                }
            }
        }
        else if (reactions[1].get_type() == sol::type::number)
        {
            actions.emplace_back(readAction(reactions));
        }

        return actions;
    }

    auto gambitsOf(CLuaBaseEntity* PLuaBaseEntity) -> pawn::CGambits*
    {
        auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
        if (PChar == nullptr || !pawn::isPawn(PChar))
        {
            ShowWarningFmt("pawn: gambit call on a non-pawn entity ({})", PLuaBaseEntity->GetBaseEntity()->getName());
            return nullptr;
        }

        auto* PController = dynamic_cast<CPawnController*>(PChar->PAI->GetController());
        return PController != nullptr ? &PController->Gambits() : nullptr;
    }
} // namespace

namespace pawn
{
    void applyStarterKit(CCharEntity* PPawn)
    {
        const auto result = lua["xi"]["player"]["charCreate"](CLuaBaseEntity(PPawn));
        if (!result.valid())
        {
            const sol::error err = result;
            ShowErrorFmt("pawn: starter kit failed for {}: {}", PPawn->getName(), err.what());
        }
    }

    void loadBrain(CCharEntity* PPawn)
    {
        auto brainTable = [&]() -> sol::optional<sol::table>
        {
            const sol::optional<sol::table> pawnTable = lua["xi"]["pawn"];
            if (!pawnTable.has_value())
            {
                return std::nullopt;
            }
            return (*pawnTable)["brain"];
        };

        // The brain library normally arrives through modules/init.txt; load
        // it directly when it hasn't
        if (!brainTable().has_value())
        {
            if (const auto loaded = lua.safe_script_file(kBrainScript); !loaded.valid())
            {
                const sol::error err = loaded;
                ShowErrorFmt("pawn: cannot load {}: {}", kBrainScript, err.what());
                return;
            }
        }

        const auto brain = brainTable();
        if (!brain.has_value())
        {
            ShowErrorFmt("pawn: {} defines no xi.pawn.brain", kBrainScript);
            return;
        }

        const sol::protected_function load = (*brain)["load"];
        if (!load.valid())
        {
            ShowErrorFmt("pawn: xi.pawn.brain.load is missing");
            return;
        }

        if (const auto result = load(CLuaBaseEntity(PPawn)); !result.valid())
        {
            const sol::error err = result;
            ShowErrorFmt("pawn: brain load failed for {}: {}", PPawn->getName(), err.what());
        }
    }
} // namespace pawn

// Bindings and hooks live apart from the pawn logic: this TU pays the sol2
// template compile cost, pawn.cpp does not.
class PawnModule : public CPPModule
{
    void OnInit() override
    {
        pawn::cleanupStaleRows();

        lua["CBaseEntity"]["pawnCreate"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return pawn::create(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };

        lua["CBaseEntity"]["pawnSpawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            return pawn::spawn(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), targetName);
        };

        lua["CBaseEntity"]["pawnDespawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::despawn(targetName);
        };

        lua["CBaseEntity"]["pawnGoto"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName, const uint16 zoneId) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::orderTravelByName(targetName, zoneId);
        };

        lua["CBaseEntity"]["pawnReloadBrain"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::reloadBrainByName(targetName);
        };

        // Gambit surface, trust vocabulary: pawn:pawnAddGambit(ai.t.PARTY,
        // { ai.c.HPP_LT, 50 }, { ai.r.MA, ai.s.HIGHEST, xi.magic.spellFamily.CURE }, retry)
        lua["CBaseEntity"]["pawnAddGambit"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint16 target, const sol::table& conditions, const sol::table& actions, const sol::object& retry) -> std::string
        {
            auto* PGambits = gambitsOf(PLuaBaseEntity);
            if (PGambits == nullptr)
            {
                return {};
            }

            Gambit_t gambit;
            gambit.target_selector  = static_cast<G_TARGET>(target);
            gambit.predicate_groups = parsePredicates(conditions);
            gambit.actions          = parseActions(actions);
            gambit.retry_delay      = retry.is<uint16>() ? retry.as<uint16>() : 0;

            if (gambit.predicate_groups.empty() || gambit.actions.empty())
            {
                ShowWarningFmt("pawn: malformed gambit for {} (target {}): no conditions or no actions", PLuaBaseEntity->GetBaseEntity()->getName(), target);
                return {};
            }

            return PGambits->AddGambit(std::move(gambit));
        };

        lua["CBaseEntity"]["pawnRemoveGambit"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& id) -> void
        {
            if (auto* PGambits = gambitsOf(PLuaBaseEntity))
            {
                PGambits->RemoveGambit(id);
            }
        };

        lua["CBaseEntity"]["pawnClearGambits"] = [](CLuaBaseEntity* PLuaBaseEntity) -> void
        {
            if (auto* PGambits = gambitsOf(PLuaBaseEntity))
            {
                PGambits->RemoveAllGambits();
            }
        };

        lua["CBaseEntity"]["pawnGambitCount"] = [](CLuaBaseEntity* PLuaBaseEntity) -> uint32
        {
            auto* PGambits = gambitsOf(PLuaBaseEntity);
            return PGambits != nullptr ? static_cast<uint32>(PGambits->Size()) : 0;
        };

        // pawn:pawnSetTPSkillSettings(ai.tp.CLOSER_UNTIL_TP, ai.s.HIGHEST, 1500)
        lua["CBaseEntity"]["pawnSetTPSkillSettings"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint16 trigger, const uint16 select, const sol::object& value) -> void
        {
            if (auto* PGambits = gambitsOf(PLuaBaseEntity))
            {
                PGambits->SetTPSkillSettings(static_cast<G_TP_TRIGGER>(trigger), static_cast<G_SELECT>(select), value.is<uint16>() ? value.as<uint16>() : 0);
            }
        };

        // Cardian management surface (!cardian command / companion addon).
        // Every call resolves the named pawn through findManagedPawn, so only
        // the summoner can inspect or move a cardian's belongings. Mutators
        // return "" on success, else a reason forwarded to the addon.
        const auto managedPair = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::pair<CCharEntity*, CCharEntity*>
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return { PChar, pawn::findManagedPawn(PChar, name) };
        };

        // The !cardian command's replies, over the Cardian Link when this
        // character's addon is bound to one: '#cd tag ...' chat lines become
        // 'cd tag ...' link lines. false = no link; the command then prints
        // to chat for a human typing it.
        lua["CBaseEntity"]["cardianLinkSend"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& line) -> bool
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return false;
            }
            const std::string wire = line.rfind("#cd ", 0) == 0 ? "cd " + line.substr(4) : line;
            return cardian::link::sendToCharacter(PChar->id, wire);
        };

        lua["CBaseEntity"]["cardianNames"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto  names = ::lua.create_table();
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar != nullptr)
            {
                for (const auto& name : pawn::managedPawnNames(PChar->id))
                {
                    names.add(name);
                }
            }
            return names;
        };

        lua["CBaseEntity"]["cardianGive"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint32 qty) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::giveToPawn(PChar, PPawn, slot, qty) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianTake"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint32 qty) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::takeFromPawn(PChar, PPawn, slot, qty) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianWear"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 invSlot, const uint8 equipSlot) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::equip(PPawn, invSlot, equipSlot) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianStrip"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 equipSlot) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::unequip(PPawn, equipSlot) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianInv"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            auto result = ::lua.create_table();
            if (const auto* storage = PPawn->getStorage(LOC_INVENTORY))
            {
                result["size"] = storage->GetSize();
                result["free"] = storage->GetFreeSlotsCount();
            }
            auto chunkTable = ::lua.create_table();
            for (const auto& chunk : pawn::items::inventoryChunks(PPawn))
            {
                chunkTable.add(chunk);
            }
            result["chunks"] = chunkTable;
            return result;
        };

        lua["CBaseEntity"]["cardianHunt"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            return pawn::setHunting(PPawn, on) ? "" : "no controller";
        };

        lua["CBaseEntity"]["cardianHomePoint"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            return pawn::homePoint(PPawn) ? "" : "not KO'd";
        };

        lua["CBaseEntity"]["cardianUse"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::useItem(PPawn, slot) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianDrop"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint32 qty) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::dropItem(PPawn, slot, qty) : "no such cardian";
        };

        // The scroll flow in one action: transfer, then the pawn uses the
        // stack from wherever it landed
        lua["CBaseEntity"]["cardianGiveUse"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint32 qty) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }

            uint8 landed = 0;
            if (auto err = pawn::items::giveToPawn(PChar, PPawn, slot, qty, &landed); !err.empty())
            {
                return err;
            }
            return pawn::items::useItem(PPawn, landed);
        };

        // Attack/defense for the companion equip screen; upstream exposes no
        // Lua accessor for the computed values
        lua["CBaseEntity"]["cardianCombatStats"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            auto stats   = ::lua.create_table();
            stats["att"] = PPawn->ATT(SLOT_MAIN);
            stats["def"] = PPawn->DEF();
            return stats;
        };

        lua["CBaseEntity"]["cardianGear"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            auto chunkTable = ::lua.create_table();
            for (const auto& chunk : pawn::items::equipChunks(PPawn))
            {
                chunkTable.add(chunk);
            }
            return chunkTable;
        };
    }

    void OnZoneTick(CZone* PZone) override
    {
        pawn::onZoneTick(PZone);
    }

    // Formation latency instrumentation: when did the client's own position
    // packet last arrive for this character (compared against the link's
    // stream age in CPawnController::LeadPoint under pawn.FORMATION_DEBUG)
    auto OnIncomingPacket(MapSession* PSession, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        std::ignore = PSession;
        if (PChar != nullptr && packet.getType() == std::to_underlying(PacketC2S::GP_CLI_COMMAND_POS))
        {
            pawn::notePositionPacket(PChar);
        }
        return false;
    }

    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (!pawn::isPawn(PChar) || packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_GROUP_SOLICIT_REQ))
        {
            return;
        }

        if (packet->ref<uint8>(0x0B) == std::to_underlying(PartyKind::Party))
        {
            pawn::noteInvite(PChar);
        }
    }
};
REGISTER_CPP_MODULE(PawnModule);
