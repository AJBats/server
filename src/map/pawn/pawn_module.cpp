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
#include "gambit_text.h"
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
#include "ability.h"
#include "recast_container.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{

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
        auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
        if (PController == nullptr)
        {
            return;
        }

        auto& gambits = PController->Gambits();
        gambits.RemoveAllGambits();

        if (pawn::loadSavedGambits(PPawn))
        {
            return;
        }

        std::size_t count = 0;
        for (const auto& [spec, enabled] : pawn::defaultRows())
        {
            if (auto row = pawn::text::parseRow(spec); row.has_value())
            {
                gambits.AddGambit(std::move(*row), enabled);
                ++count;
            }
            else
            {
                ShowErrorFmt("pawn: malformed default row '{}'", spec);
            }
        }
        ShowInfoFmt("pawn: default gambits loaded for {} ({} rows)", PPawn->getName(), count);
    }
} // namespace pawn

// Bindings and hooks live apart from the pawn logic: this TU pays the sol2
// template compile cost, pawn.cpp does not.
class PawnModule : public CPPModule
{
    void OnInit() override
    {
        pawn::cleanupStaleRows();

        // The Cardian-only gambit vocabulary, published once from the C++
        // definitions so the brains cannot drift from the interpreter
        lua["xi"]["pawn"]             = lua["xi"]["pawn"].get_or_create<sol::table>();
        lua["xi"]["pawn"]["r"]        = lua.create_table_with("BEHAVIOR", static_cast<uint16>(pawn::G_REACTION_BEHAVIOR));
        lua["xi"]["pawn"]["c"]        = lua.create_table_with("STRATEGY", static_cast<uint16>(pawn::G_CONDITION_STRATEGY));
        lua["xi"]["pawn"]["behavior"] = lua.create_table_with("AVOID_AGGRO", static_cast<uint16>(pawn::Behavior::AvoidAggro),
                                                              "FORMATION", static_cast<uint16>(pawn::Behavior::Formation),
                                                              "REST_WITH_PLAYER", static_cast<uint16>(pawn::Behavior::RestWithPlayer),
                                                              "HOME_POINT_WITH_PLAYER", static_cast<uint16>(pawn::Behavior::HomePointWithPlayer));
        lua["xi"]["pawn"]["slot"]     = lua.create_table_with("FOLLOW", static_cast<uint16>(pawn::Slot::Follow),
                                                              "LEAD", static_cast<uint16>(pawn::Slot::Lead),
                                                              "FLANK_LEFT", static_cast<uint16>(pawn::Slot::FlankLeft),
                                                              "FLANK_RIGHT", static_cast<uint16>(pawn::Slot::FlankRight),
                                                              "REAR_LEFT", static_cast<uint16>(pawn::Slot::RearLeft),
                                                              "REAR_RIGHT", static_cast<uint16>(pawn::Slot::RearRight),
                                                              "BEHIND", static_cast<uint16>(pawn::Slot::Behind));

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

            // A behaviour switch is a row of its own: mixed with a real
            // action it would be silently ignored by the action interpreter
            const auto behaviors = std::count_if(gambit.actions.begin(), gambit.actions.end(), [](const Action_t& a)
                                                 {
                                                     return a.reaction == pawn::G_REACTION_BEHAVIOR;
                                                 });
            if (behaviors != 0 && static_cast<std::size_t>(behaviors) != gambit.actions.size())
            {
                ShowWarningFmt("pawn: malformed gambit for {} (target {}): a BEHAVIOR action cannot share a row with other actions", PLuaBaseEntity->GetBaseEntity()->getName(), target);
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

        lua["CBaseEntity"]["cardianAccountPawns"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto names = ::lua.create_table();
            for (const auto& name : pawn::accountPawnNames(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity())))
            {
                names.add(name);
            }
            return names;
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

        // The gambit editor's view of a cardian's rows (M3.85): index, on,
        // the row in the grammar, and the label as the player reads it
        const auto gambitsOf = [](CCharEntity* PPawn) -> pawn::CGambits*
        {
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr ? &PController->Gambits() : nullptr;
        };
        lua["CBaseEntity"]["cardianGambits"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            auto* PGambits            = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return sol::lua_nil;
            }
            auto result   = ::lua.create_table();
            auto rows     = ::lua.create_table();
            std::size_t n = 0;
            for (const auto& row : PGambits->Rows())
            {
                ++n;
                auto entry     = ::lua.create_table();
                entry["index"] = n;
                entry["on"]    = row.enabled;
                entry["spec"]  = pawn::text::formatRow(row.gambit);
                entry["label"] = pawn::labelGambit(row.gambit);
                rows.add(entry);
            }
            result["master"] = PGambits->MasterOn();
            result["rows"]   = rows;
            return result;
        };
        // Every edit saves the set (cardian_gambits)
        lua["CBaseEntity"]["cardianGambitToggle"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const bool on) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            if (!PGambits->SetEnabled(index, on))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitMove"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 from, const uint32 to) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            if (!PGambits->Move(from, to))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitDelete"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            if (!PGambits->Erase(index))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitInsert"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const std::string& spec) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            auto gambit = pawn::text::parseRow(spec);
            if (!gambit.has_value())
            {
                return "malformed row";
            }
            if (!PGambits->Insert(index, std::move(*gambit)))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitReplace"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const std::string& spec) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            auto gambit = pawn::text::parseRow(spec);
            if (!gambit.has_value())
            {
                return "malformed row";
            }
            if (!PGambits->Replace(index, std::move(*gambit)))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitVocab"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            auto* PPawn = managedPair(PLuaBaseEntity, name).second;
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }
            const auto vocab  = pawn::vocabularyFor(PPawn);
            auto       result = ::lua.create_table();
            const auto pack   = [](const std::vector<pawn::VocabEntry>& entries)
            {
                auto list = ::lua.create_table();
                for (const auto& e : entries)
                {
                    auto entry     = ::lua.create_table();
                    entry["key"]     = e.key;
                    entry["label"]   = e.label;
                    entry["group"]   = e.group;
                    entry["targets"] = e.targets;
                    list.add(entry);
                }
                return list;
            };
            result["targets"]    = pack(vocab.targets);
            result["conditions"] = pack(vocab.conditions);
            result["statuses"]   = pack(vocab.statuses);
            result["actions"]    = pack(vocab.actions);
            return result;
        };
        lua["CBaseEntity"]["cardianGambitMaster"] = [managedPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            auto* PPawn    = managedPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            PGambits->SetMaster(on);
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitReset"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            pawn::forgetGambits(PPawn);
            return pawn::reloadBrain(PPawn) ? "" : "no such cardian";
        };

        lua["CBaseEntity"]["cardianHunt"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            // A flag, never a gambit row: the lead slot is the list's call
            return pawn::setHunting(PPawn, on) ? "" : "no controller";
        };

        // The party strategy channel: orders live on the player and every
        // cardian of theirs follows them
        lua["CBaseEntity"]["cardianOrders"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::object
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return sol::lua_nil;
            }
            const auto rules     = pawn::huntRulesOf(PChar->id);
            auto       result    = ::lua.create_table();
            result["strategy"]   = pawn::strategyOf(PChar->id);
            result["retreat"]    = pawn::isRetreating(PChar->id);
            result["hunt_min"]   = rules.minCheck;
            result["hunt_max"]   = rules.maxCheck;
            result["pull_first"] = rules.pullFirst;
            result["aggressive"] = rules.aggressive;
            result["links"]      = rules.links;
            auto names           = ::lua.create_table();
            for (uint16 i = 0; i < pawn::kStrategyCount; ++i)
            {
                names.add(std::string(pawn::strategyName(i)));
            }
            result["names"] = names;
            return result;
        };
        lua["CBaseEntity"]["cardianSetStrategy"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint16 strategy) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            if (strategy >= pawn::kStrategyCount)
            {
                return "no such strategy";
            }
            pawn::setStrategy(PChar, strategy);
            return "";
        };
        lua["CBaseEntity"]["cardianSetHunt"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& field, const int value) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return pawn::setHuntRule(PChar, field, value);
        };
        // Wait here / follow me. Follow from another zone is a travel order
        // to the player's: she treks the world to meet them
        lua["CBaseEntity"]["cardianWait"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            if (PController == nullptr)
            {
                return "she is not herself right now";
            }
            PController->SetWaiting(on, true);
            if (on)
            {
                pawn::clearTravelOrder(PPawn->id);
                ShowInfoFmt("pawn: {} waits here (ordered)", PPawn->getName());
            }
            else if (PPawn->loc.zone != PChar->loc.zone)
            {
                ShowInfoFmt("pawn: {} sets out to meet {} in zone {}", PPawn->getName(), PChar->getName(), static_cast<uint16>(PChar->getZone()));
                pawn::orderTravelByName(name, static_cast<uint16>(PChar->getZone()));
            }
            else
            {
                ShowInfoFmt("pawn: {} follows (ordered)", PPawn->getName());
            }
            return "";
        };

        lua["CBaseEntity"]["cardianWaiting"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            const auto* PController   = PPawn != nullptr ? dynamic_cast<const CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr && PController->IsWaiting();
        };

        lua["CBaseEntity"]["cardianRetreat"] = [](CLuaBaseEntity* PLuaBaseEntity, const bool on) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            pawn::setRetreat(PChar, on);
            return "";
        };
        lua["CBaseEntity"]["cardianEngage"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint16 targid) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return pawn::partyEngage(PChar, targid);
        };

        lua["CBaseEntity"]["cardianAvoid"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            if (!pawn::setBehaviorRow(PPawn, pawn::Behavior::AvoidAggro, on ? 1 : 0))
            {
                return "no controller";
            }
            pawn::saveGambits(PPawn);
            return "";
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

        // What she cannot do yet and for how long: the seconds left on
        // every spell and ability still on recast, keyed the way the
        // vocabulary keys them so a command list can label its own rows.
        // Only what is actually waiting is sent; the rest are ready.
        lua["CBaseEntity"]["cardianRecasts"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            auto       table = ::lua.create_table();
            const auto now   = timer::now();
            const auto left  = [&](const Recast_t& recast) -> double
            {
                auto remaining = (recast.TimeStamp + recast.RecastTime) - now;
                // A charged ability is usable while any charge is back, so
                // only the wait for the next charge counts: the recast holds
                // every spent charge's time end to end, and the ability is
                // ready once fewer than all but one remain (HasRecast)
                if (recast.chargeTime != 0s && recast.maxCharges > 0)
                {
                    remaining -= recast.chargeTime * (recast.maxCharges - 1);
                }
                return remaining > 0s ? std::chrono::duration<double>(remaining).count() : 0.0;
            };

            if (auto* PList = PPawn->PRecastContainer->GetRecastList(RECAST_MAGIC); PList != nullptr)
            {
                for (const auto& recast : *PList)
                {
                    if (const auto seconds = left(recast); seconds > 0.0)
                    {
                        table[fmt::format("2:2:{}", static_cast<uint16>(recast.ID))] = seconds;
                    }
                }
            }

            // Abilities are stored by recast id, the vocabulary keys them by
            // ability id, so they are matched through her own ability list
            if (auto* PList = PPawn->PRecastContainer->GetRecastList(RECAST_ABILITY); PList != nullptr)
            {
                for (const auto job : { PPawn->GetMJob(), PPawn->GetSJob() })
                {
                    for (auto* PAbility : ability::GetAbilities(job))
                    {
                        if (PAbility == nullptr || !charutils::hasAbility(PPawn, PAbility->getID()))
                        {
                            continue;
                        }
                        for (const auto& recast : *PList)
                        {
                            if (recast.ID != PAbility->getRecastId())
                            {
                                continue;
                            }
                            if (const auto seconds = left(recast); seconds > 0.0)
                            {
                                table[fmt::format("3:2:{}", PAbility->getID())] = seconds;
                            }
                        }
                    }
                }
            }
            return table;
        };

        // Her experience on her main job, and what the next level costs:
        // the character's own screens show it, and there is no upstream
        // getter for either
        lua["CBaseEntity"]["cardianExp"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            const auto job   = static_cast<uint8>(PPawn->GetMJob());
            auto       table = ::lua.create_table();
            table["exp"]     = job < MAX_JOBTYPE ? PPawn->jobs.exp[job] : 0;
            table["tnl"]     = charutils::GetExpNEXTLevel(PPawn->GetMLevel());
            return table;
        };

        // A cardian, for the Lua module that moves quest and mission
        // progress with the party (modules/cardian/lua/party_progress.lua)
        lua["CBaseEntity"]["isCardian"] = [](CLuaBaseEntity* PLuaBaseEntity) -> bool
        {
            const auto* PChar = dynamic_cast<const CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return PChar != nullptr && pawn::isPawn(PChar);
        };

        // The command window: one action now, on a target index in the
        // zone (0 = herself)
        lua["CBaseEntity"]["cardianDo"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& key, const uint16 targid) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            if (PPawn->loc.zone == nullptr)
            {
                return "not in a zone";
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            if (PController == nullptr)
            {
                return "no controller";
            }
            CBattleEntity* PTarget = targid == 0 ? static_cast<CBattleEntity*>(PPawn)
                                                 : dynamic_cast<CBattleEntity*>(PPawn->loc.zone->GetEntity(targid, TYPE_PC | TYPE_MOB | TYPE_NPC));
            if (PTarget == nullptr)
            {
                return "no such target";
            }
            const auto err = PController->DoAction(key, PTarget);
            if (err.empty())
            {
                ShowInfoFmt("pawn: {} does {} on {} ({}'s order)", PPawn->getName(), key, PTarget->getName(), PChar->getName());
            }
            return err;
        };

        lua["CBaseEntity"]["cardianRescue"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::rescue(PChar, PPawn) : "no such cardian";
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

        // The Profile page: what the client's own Profile screen shows --
        // title, nation, race, home point, rank and rank points
        lua["CBaseEntity"]["cardianProfile"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            const auto nation = std::min<uint8>(PPawn->profile.nation, 2);
            auto*      PZone  = zoneutils::GetZone(PPawn->profile.home_point.destination);

            auto table          = ::lua.create_table();
            table["title"]      = PPawn->profile.title;
            table["nation"]     = nation;
            table["race"]       = PPawn->look.race;
            table["rank"]       = PPawn->profile.rank[nation];
            table["rankpoints"] = PPawn->profile.rankpoints;
            table["home"]       = PZone != nullptr ? PZone->getName() : std::string("?");
            return table;
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
