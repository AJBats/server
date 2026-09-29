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

#include "auction.h"
#include "cardian_link.h"
#include "engage_math.h"
#include "pawn.h"
#include "party_finder.h"
#include "seats.h"
#include "world.h"
#include "pawn_controller.h"
#include "gambit_text.h"
#include "link_api.h"
#include "pawn_gambits.h"
#include "spell_bank.h"
#include "tactics.h"
#include "view.h"

#include "common/logging.h"

#include "ai/ai_container.h"
#include "entities/char_entity.h"
#include "pause/pause.h"
#include "enums/packet_c2s.h"
#include "enums/packet_s2c.h"
#include "enums/party_kind.h"
#include "packets/c2s/0x06e_group_solicit_req.h"
#include "lua/lua_base_entity.h"
#include "lua/luautils.h"
#include "packets/basic.h"
#include "utils/moduleutils.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <magic_enum/magic_enum.hpp>
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

        // charCreate marks a new adventurer; a cardian is none
        PPawn->playerConfig.NewAdventurerOffFlg = true;
        charutils::SavePlayerSettings(PPawn);
    }

    void loadBrain(CCharEntity* PPawn)
    {
        auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
        if (PController == nullptr)
        {
            return;
        }

        // Her own rows, the same list wherever she is. A world body's world
        // layer is not among them and is left as it is: it runs ahead of them
        // while she is in the wild (CGambits, gambit_layers.h), and rebuilds
        // by itself when the world's file, her job or her role changes
        auto& gambits = PController->Gambits();
        gambits.RemoveAllGambits();

        // The set she has, else her job's defaults (gambit_defaults.h),
        // seeded once. A job change leaves them as they are.
        if (pawn::loadSavedGambits(PPawn))
        {
            return;
        }

        std::size_t count = 0;
        for (const auto& [spec, enabled] : pawn::defaultRowsFor(PPawn->GetMJob()))
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

        // A seeded set has her gambits on, whatever they were before it:
        // a guest the player switched off is herself again once she leaves
        PController->SetOwnMaster(true);

        const auto job = magic_enum::enum_name(PPawn->GetMJob());
        if (pawn::world::hasBody(PPawn->id))
        {
            // Nothing of a world body's is saved: she seeds at every stand
            // from the job the census gave her, which she keeps, and a set
            // she has is a guest's -- the player's edits while she was in
            // his party -- which goes with her contract (forgetGuestGambits)
            ShowInfoFmt("pawn: {} default gambits seeded for {} ({} rows, a world body's: not saved)", job, PPawn->getName(), count);
        }
        else
        {
            // Saved at once, so a job change leaves her rows in place
            pawn::saveGambits(PPawn);
            ShowInfoFmt("pawn: {} default gambits seeded for {} ({} rows, saved)", job, PPawn->getName(), count);
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
        // The cardian API's messages on the Cardian Link (link_api.cpp), and
        // the Lua libraries two of them are answered from
        pawn::linkapi::registerHandlers();
        pawn::linkapi::loadLibraries();
        // The seat waterfall (ROADMAP H): the ladder, its lookups and its engine
        pawn::seats::init();
        // The MP bank's samplers, a Lua library (RESEARCH §12.13)
        pawn::tactics::bank::load();

        // The Cardian-only gambit vocabulary, published once from the C++
        // definitions so the brains cannot drift from the interpreter
        lua["xi"]["pawn"]             = lua["xi"]["pawn"].get_or_create<sol::table>();
        lua["xi"]["pawn"]["r"]        = lua.create_table_with("BEHAVIOR", static_cast<uint16>(pawn::G_REACTION_BEHAVIOR));
        lua["xi"]["pawn"]["c"]        = lua.create_table_with("STRATEGY", static_cast<uint16>(pawn::G_CONDITION_STRATEGY));
        lua["xi"]["pawn"]["behavior"] = lua.create_table_with("AVOID_AGGRO", static_cast<uint16>(pawn::Behavior::AvoidAggro),
                                                              "AVOID_LINKS", static_cast<uint16>(pawn::Behavior::AvoidLinks),
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

        // The world's adventurers (ROADMAP D0): stand, fade, ring, walk
        lua["CBaseEntity"]["worldSpawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                return false;
            }
            return pawn::world::spawnByName(name, PChar->loc.zone, PChar->loc.p, false);
        };

        lua["CBaseEntity"]["worldDespawn"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> uint32
        {
            std::ignore = PLuaBaseEntity;
            return pawn::world::despawnByName(name);
        };

        lua["CBaseEntity"]["worldRing"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint32 count) -> uint32
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                return 0;
            }
            return pawn::world::ring(PChar->loc.zone, PChar->loc.p, count, false);
        };

        lua["CBaseEntity"]["worldFarm"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> uint32
        {
            std::ignore = PLuaBaseEntity;
            return pawn::world::farm(name, on);
        };

        // The slot tables (ROADMAP D3): the zone's slots, a refill, a slot authored where you stand
        lua["CBaseEntity"]["worldSlots"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            auto  lines = ::lua.create_table();
            if (PChar != nullptr && PChar->loc.zone != nullptr)
            {
                for (const auto& line : pawn::world::slots(PChar->loc.zone))
                {
                    lines.add(line);
                }
            }
            return lines;
        };

        lua["CBaseEntity"]["worldFill"] = [](CLuaBaseEntity* PLuaBaseEntity) -> uint32
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                return 0;
            }
            return pawn::world::fill(PChar->loc.zone);
        };

        lua["CBaseEntity"]["worldSlot"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& activity, const uint8 low, const uint8 high, const uint8 count, const float spread) -> bool
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                return false;
            }
            return pawn::world::addSlot(PChar->loc.zone, activity, low, high, count, spread, PChar->loc.p);
        };

        lua["CBaseEntity"]["pawnGoto"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& targetName, const uint16 zoneId) -> bool
        {
            std::ignore = PLuaBaseEntity;
            return pawn::orderTravelByName(targetName, zoneId, 0);
        };

        // The club signs in with the player (ROADMAP H): the chat line,
        // "Jevyak (Northern San d'Oria), Zapp (Southern San d'Oria)", or ""
        lua["CBaseEntity"]["cardianSignIn"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return PChar != nullptr ? pawn::signInClub(PChar) : std::string{};
        };

        // Recruit and release (ROADMAP H): the debug verbs behind the real ones
        lua["CBaseEntity"]["cardianRecruit"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            return pawn::recruitCardian(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), name);
        };

        lua["CBaseEntity"]["cardianRelease"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            return pawn::releaseCardian(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), name);
        };

        // The waterfall's caps, live -- standing is server-wide, faded is
        // per zone. 0 and 0 puts the settings back
        lua["CBaseEntity"]["worldCap"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint32 standing, const uint32 faded) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            pawn::seats::setCaps(standing, faded);
            return pawn::seats::capsLine(PChar != nullptr ? static_cast<uint16>(PChar->getZone()) : 0);
        };

        lua["CBaseEntity"]["worldCaps"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return pawn::seats::capsLine(PChar != nullptr ? static_cast<uint16>(PChar->getZone()) : 0);
        };

        lua["CBaseEntity"]["cardianFaded"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            auto  names = ::lua.create_table();
            if (PChar != nullptr)
            {
                for (const auto& name : pawn::seats::fadedNames(PChar->id))
                {
                    names.add(name);
                }
            }
            return names;
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

            // The grammar's and the editor's refusals hold here too: a
            // retired behaviour never comes back, and no row is silently dead
            if (std::ranges::any_of(gambit.actions, [](const Action_t& a)
                                    {
                                        return a.reaction == pawn::G_REACTION_BEHAVIOR && pawn::isRetiredBehavior(static_cast<uint32>(a.select));
                                    }))
            {
                ShowWarningFmt("pawn: malformed gambit for {} (target {}): a retired behaviour", PLuaBaseEntity->GetBaseEntity()->getName(), target);
                return {};
            }
            if (const auto why = cardian::engage::pairingError(gambit); !why.empty())
            {
                ShowWarningFmt("pawn: malformed gambit for {} (target {}): {}", PLuaBaseEntity->GetBaseEntity()->getName(), target, why);
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

        // Cardian management surface for the debug commands and the party
        // progress module (modules/cardian/lua/party_progress.lua).
        // Two gates (ROADMAP H: command yes, manage no). managedPair resolves
        // the named pawn through findManagedPawn: only the summoner inspects
        // or moves her belongings or spends her money. commandPair resolves
        // through findCommandablePawn: summoned or in the player's party, so
        // a wild cardian invited along takes orders, shows what /check would
        // show, is sent home when KO'd, and has her gambits edited as a
        // guest's -- cleared when she leaves the party (pawn::leftParty).
        // Mutators return "" on success, else a reason the command prints.
        const auto managedPair = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::pair<CCharEntity*, CCharEntity*>
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return { PChar, pawn::findManagedPawn(PChar, name) };
        };
        const auto commandPair = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::pair<CCharEntity*, CCharEntity*>
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return { PChar, pawn::findCommandablePawn(PChar, name) };
        };

        lua["CBaseEntity"]["cardianBond"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& why, sol::optional<bool> mission)
        {
            const auto* PPlayer = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PPlayer != nullptr)
            {
                pawn::finder::bond(PPlayer->id, charutils::getCharIdFromName(name), why.c_str(), mission.value_or(false));
            }
        };
        // Her contract with the given player (the cardian's own entity asks)
        lua["CBaseEntity"]["cardianContract"] = [](CLuaBaseEntity* PLuaBaseEntity, const uint32 playerCharID) -> std::string
        {
            const auto* PMember = PLuaBaseEntity != nullptr ? PLuaBaseEntity->GetBaseEntity() : nullptr;
            return PMember != nullptr ? pawn::finder::contractWith(PMember->id, playerCharID) : "";
        };
        // The fight log as it stands (!tactics, RESEARCH §12.5)
        lua["CBaseEntity"]["cardianTactics"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            sol::table out = ::lua.create_table();
            auto*      PChar = PLuaBaseEntity != nullptr ? dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()) : nullptr;
            for (const auto& line : pawn::tactics::lines(PChar))
            {
                out.add(line);
            }
            return out;
        };
        // Hers to manage, or only to command
        lua["CBaseEntity"]["cardianOwns"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            return managedPair(PLuaBaseEntity, name).second != nullptr;
        };

        lua["CBaseEntity"]["cardianHunt"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            // A flag, never a gambit row: the lead slot is the list's call
            return pawn::setHunting(PPawn, on) ? "" : "no controller";
        };

        lua["CBaseEntity"]["cardianAvoid"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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

        // Sent home alone, to the ordering player's home point (!pawnhomepoint;
        // the addon sends the Link's HOMEPOINT)
        lua["CBaseEntity"]["cardianHomePoint"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            return pawn::orderHomePoint(PChar, PPawn) == CL_S_OK ? "" : "not KO'd";
        };

        // A cardian, for the Lua module that moves quest and mission
        // progress with the party (modules/cardian/lua/party_progress.lua)
        lua["CBaseEntity"]["isCardian"] = [](CLuaBaseEntity* PLuaBaseEntity) -> bool
        {
            const auto* PChar = dynamic_cast<const CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return PChar != nullptr && pawn::isPawn(PChar);
        };

        // The steer tick (pawn/view.h, every kSteerPeriodMs): every cardian
        // under a walk order takes her step
        cardian::view::setSteerTick([]()
        {
            pawn::forEachWalkOrder([](const uint32 charid)
            {
                auto* PPawn = zoneutils::GetChar(charid);
                if (PPawn == nullptr || PPawn->PAI == nullptr)
                {
                    return;
                }
                if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()))
                {
                    PController->WalkStep();
                }
            });
        });
    }

    // A character entering a zone -- a login, a zone change, a cardian's
    // stand -- is a body the fight log's hitch must be on if she is routed
    void OnCharZoneIn(CCharEntity* PChar) override
    {
        pawn::tactics::zoneIn(PChar);
    }

    // A held simulation (pause/pause.h) takes no step here either: the world's
    // bodies, the seat ladder and the tacticians wait, and only the outboxes drain.
    void OnZoneTick(CZone* PZone) override
    {
        if (cardian::pause::isHeld())
        {
            pawn::onZoneTickHeld(PZone);
            return;
        }

        pawn::onZoneTick(PZone);
    }

    // Formation latency instrumentation: when did the client's own position
    // packet last arrive for this character (compared against the link's
    // stream age in CPawnController::LeadPoint under pawn.FORMATION_DEBUG).
    // And the game's own party invite (UniqueNo is the invitee's charid
    // whether she was targeted or named): one of the world's adventurers
    // is refused and the packet dropped, a faded cardian of the player's
    // own is stood so the handler finds her
    auto OnIncomingPacket(MapSession* PSession, CCharEntity* PChar, CBasicPacket& packet) -> bool override
    {
        std::ignore = PSession;
        if (PChar == nullptr)
        {
            return false;
        }
        if (packet.getType() == std::to_underlying(PacketC2S::GP_CLI_COMMAND_POS))
        {
            pawn::notePositionPacket(PChar);
        }
        else if (packet.getType() == std::to_underlying(PacketC2S::GP_CLI_COMMAND_GROUP_SOLICIT_REQ))
        {
            if (const auto* solicit = packet.as<GP_CLI_COMMAND_GROUP_SOLICIT_REQ>(); solicit->Kind == PartyKind::Party)
            {
                return pawn::finder::interceptInvite(PChar, solicit->UniqueNo);
            }
        }
        return false;
    }

    void OnPushPacket(CCharEntity* PChar, const std::unique_ptr<CBasicPacket>& packet) override
    {
        if (!pawn::isPawn(PChar))
        {
            return;
        }

        // What the game tells her in a battle message: the message number at 0x18,
        // the index of whom it is about at 0x16
        if (packet->getType() == std::to_underlying(PacketS2C::GP_SERV_COMMAND_BATTLE_MESSAGE))
        {
            pawn::noteBattleMessage(PChar, packet->ref<uint16>(0x18), packet->ref<uint16>(0x16));
            return;
        }

        if (packet->getType() != std::to_underlying(PacketS2C::GP_SERV_COMMAND_GROUP_SOLICIT_REQ))
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
