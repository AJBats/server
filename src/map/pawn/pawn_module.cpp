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
#include "pawn_gambits.h"
#include "pawn_items.h"
#include "spell_bank.h"
#include "tactics.h"
#include "view.h"

#include "common/logging.h"

#include "ai/ai_container.h"
#include "entities/char_entity.h"
#include "pause/pause.h"
#include "enums/packet_c2s.h"
#include "enums/packet_s2c.h"
#include "item_container.h"
#include "enums/party_kind.h"
#include "packets/c2s/0x06e_group_solicit_req.h"
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
#include <cctype>
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
        // seeded once. A job change leaves them as they are; a reset
        // (greset) seeds them again from the job she holds then.
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

        // A cardian of yours without a body stands again: to the front of
        // her tier and a run. Not a managedPair: she has no body to find
        lua["CBaseEntity"]["cardianRecall"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            return PChar != nullptr ? pawn::seats::recall(PChar->id, name) : "nobody is asking";
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

        // Cardian management surface (!cardian command / companion addon).
        // Two gates (ROADMAP H: command yes, manage no). managedPair resolves
        // the named pawn through findManagedPawn: only the summoner inspects
        // or moves her belongings or spends her money. commandPair resolves
        // through findCommandablePawn: summoned or in the player's party, so
        // a wild cardian invited along takes orders, shows what /check would
        // show, is sent home when KO'd, and has her gambits edited as a
        // guest's -- cleared when she leaves the party (pawn::leftParty).
        // Mutators return "" on success, else a reason forwarded to the addon.
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
        // The roster: every cardian this character commands
        lua["CBaseEntity"]["cardianNames"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto names = ::lua.create_table();
            for (const auto& name : pawn::commandablePawnNames(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity())))
            {
                names.add(name);
            }
            return names;
        };
        // The party finder: who is in reach and what she says to the goal
        // (exp, or a mission log or quest area), the shout, a look at a
        // responder, the invite, and the contracts
        const auto candidateRow = [](const pawn::finder::Candidate& c) -> sol::table
        {
            auto row        = ::lua.create_table();
            row["name"]     = c.name;
            row["job"]      = c.job;
            row["level"]    = c.level;
            row["zone"]     = c.zone;
            row["state"]    = c.state;
            row["willing"]  = c.answer.yes;
            row["line"]     = c.answer.line;
            row["affinity"] = c.affinity;
            row["mission"]  = static_cast<uint8>(c.answer.fit); // 0 free, 1 behind, 2 on it, 3 done it
            row["race"]     = c.race;
            row["nation"]   = c.nation;
            row["rank"]     = c.rank;
            row["zoneid"]   = c.zoneId;
            return row;
        };
        lua["CBaseEntity"]["cardianFinder"] = [candidateRow](CLuaBaseEntity* PLuaBaseEntity, const std::string& kind, const int log) -> sol::table
        {
            auto rows = ::lua.create_table();
            for (const auto& c : pawn::finder::candidates(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), pawn::finder::goalFrom(kind, log)))
            {
                rows.add(candidateRow(c));
            }
            return rows;
        };
        lua["CBaseEntity"]["cardianShout"] = [candidateRow](CLuaBaseEntity* PLuaBaseEntity, const std::string& kind, const int log, const bool again) -> sol::table
        {
            auto        out = ::lua.create_table();
            std::string why;
            const auto* made = pawn::finder::shout(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), pawn::finder::goalFrom(kind, log), again, why);
            if (made == nullptr)
            {
                out["err"] = why;
                return out;
            }
            out["id"]   = made->id;
            out["wait"] = made->waitMs;
            out["kind"] = pawn::finder::kindName(made->goal);
            out["log"]  = made->goal.log;
            auto rows   = ::lua.create_table();
            for (const auto& r : made->rows)
            {
                auto row      = candidateRow(r.c);
                row["reveal"] = r.revealMs;
                row["decide"] = r.decideMs;
                rows.add(row);
            }
            out["rows"] = rows;
            return out;
        };
        lua["CBaseEntity"]["cardianPeek"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto p = pawn::finder::peek(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), name);
            if (!p.has_value())
            {
                return sol::lua_nil;
            }
            auto t        = ::lua.create_table();
            t["name"]     = p->name;
            t["job"]      = p->job;
            t["level"]    = p->level;
            t["sjob"]     = p->sjob;
            t["slvl"]     = p->slvl;
            t["nation"]   = p->nation;
            t["rank"]     = p->rank;
            t["standing"] = p->standing;
            t["affinity"] = p->affinity;
            t["hp"]       = p->hp;
            t["maxhp"]    = p->maxhp;
            t["mp"]       = p->mp;
            t["maxmp"]    = p->maxmp;
            t["stats"]    = p->stats;
            auto gear     = ::lua.create_table();
            for (const auto& chunk : p->gear)
            {
                gear.add(chunk);
            }
            t["gear"] = gear;
            return t;
        };
        lua["CBaseEntity"]["cardianInvite"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& kind, const int log) -> std::string
        {
            return pawn::finder::invite(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), name, pawn::finder::goalFrom(kind, log));
        };
        lua["CBaseEntity"]["cardianBond"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& why, sol::optional<bool> mission)
        {
            const auto* PPlayer = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PPlayer != nullptr)
            {
                pawn::finder::bond(PPlayer->id, charutils::getCharIdFromName(name), why.c_str(), mission.value_or(false));
            }
        };
        // Your contract (ROADMAP H, the party waits): his open contracts,
        // each with her job, level, where she is, and whether she stands in
        // his party, stands waiting, or could not stand ("out")
        lua["CBaseEntity"]["cardianContracts"] = [](CLuaBaseEntity* PLuaBaseEntity) -> sol::table
        {
            auto        rows    = ::lua.create_table();
            const auto* PPlayer = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PPlayer == nullptr)
            {
                return rows;
            }
            for (const auto& c : pawn::finder::openContracts(PPlayer->id))
            {
                auto        row   = ::lua.create_table();
                const auto* PPawn = pawn::findPawn(c.charid);
                row["name"]       = c.name;
                row["kind"]       = pawn::finder::kindName(c.goal);
                if (PPawn != nullptr)
                {
                    row["job"]   = static_cast<uint8>(PPawn->GetMJob());
                    row["level"] = PPawn->GetMLevel();
                    row["zone"]  = static_cast<uint16>(PPawn->getZone());
                    row["state"] = PPawn->PParty != nullptr && PPawn->PParty == PPlayer->PParty ? "party" : "standing";
                }
                else if (const auto rset = db::preparedStmt("SELECT s.mjob, s.mlvl, c.pos_zone FROM chars c JOIN char_stats s ON s.charid = c.charid WHERE c.charid = ?", c.charid);
                         rset && rset->next())
                {
                    row["job"]   = rset->get<uint8>("mjob");
                    row["level"] = rset->get<uint8>("mlvl");
                    row["zone"]  = rset->get<uint16>("pos_zone");
                    row["state"] = pawn::seats::has(c.charid) ? "faded" : "out";
                }
                rows.add(row);
            }
            return rows;
        };
        lua["CBaseEntity"]["cardianEndContract"] = [](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            return pawn::finder::release(dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity()), name);
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
        // The stats line's tokens for a cardian the player commands
        lua["CBaseEntity"]["cardianStatsLine"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }
            return sol::make_object(::lua, pawn::finder::statsLine(PPawn));
        };
        // Hers to manage, or only to command
        lua["CBaseEntity"]["cardianOwns"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            return managedPair(PLuaBaseEntity, name).second != nullptr;
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

        // The trade window's gil line: to her, or back from her
        lua["CBaseEntity"]["cardianGil"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 amount, const bool toPawn) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::moveGil(PChar, PPawn, amount, toPawn) : "no such cardian";
        };

        // location: the inventory (0) or a wardrobe the piece is worn from
        lua["CBaseEntity"]["cardianWear"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 invSlot, const uint8 equipSlot, const uint8 location) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::equip(PPawn, invSlot, equipSlot, location) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianStrip"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 equipSlot) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::unequip(PPawn, equipSlot) : "no such cardian";
        };

        // One of her containers: the inventory (location 0) or a storage bag
        lua["CBaseEntity"]["cardianInv"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 location) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr || location > LOC_WARDROBE8)
            {
                return sol::lua_nil;
            }

            auto result = ::lua.create_table();
            if (const auto* storage = PPawn->getStorage(location))
            {
                result["size"] = storage->GetSize();
                result["free"] = storage->GetFreeSlotsCount();
            }
            auto chunkTable = ::lua.create_table();
            for (const auto& chunk : pawn::items::containerChunks(PPawn, location))
            {
                chunkTable.add(chunk);
            }
            result["chunks"] = chunkTable;
            return result;
        };

        // Her storage bags, { loc, size, used } each, in cycling order
        lua["CBaseEntity"]["cardianBags"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return sol::lua_nil;
            }

            auto result = ::lua.create_table();
            for (const auto& bag : pawn::items::bags(PPawn))
            {
                auto entry    = ::lua.create_table();
                entry["loc"]  = bag.location;
                entry["size"] = bag.size;
                entry["used"] = bag.used;
                result.add(entry);
            }
            return result;
        };

        // The Auction House screen (ROADMAP L): what the auction house has,
        // in stock or sold out, that a member of his party could wear in an
        // equipment slot -- the player
        // himself, or a cardian of his to manage (a wild one's gear is the
        // world's) -- each { id, level, stock, going, category }; nil for anyone else
        lua["CBaseEntity"]["cardianAuctionList"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 equipSlot) -> sol::object
        {
            auto*        PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            CCharEntity* PWho  = PChar != nullptr && PChar->getName() == name ? PChar : managedPair(PLuaBaseEntity, name).second;
            if (PWho == nullptr)
            {
                return sol::lua_nil;
            }

            auto result = ::lua.create_table();
            for (const auto& listing : pawn::auction::wearableAtAuction(PWho, equipSlot))
            {
                auto entry        = ::lua.create_table();
                entry["id"]       = listing.itemId;
                entry["level"]    = listing.level;
                entry["stock"]    = listing.stock;
                entry["going"]    = listing.going;
                entry["category"] = listing.category;
                result.add(entry);
            }
            return result;
        };

        lua["CBaseEntity"]["cardianMove"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 fromLoc, const uint8 slot, const uint8 toLoc, const uint32 qty) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::moveItem(PPawn, fromLoc, slot, toLoc, qty) : "no such cardian";
        };

        // The gambit editor's view of a cardian's rows (M3.85): index, on,
        // what the row means where it sits (tactician_line.h token), the row
        // in the grammar, and the label as the player reads it
        const auto gambitsOf = [](CCharEntity* PPawn) -> pawn::CGambits*
        {
            auto* PController = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr ? &PController->Gambits() : nullptr;
        };
        lua["CBaseEntity"]["cardianGambits"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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
                entry["state"] = std::string(cardian::tactician::token(PGambits->StateOf(n)));
                entry["spec"]  = pawn::text::formatRow(row.gambit);
                entry["label"] = pawn::labelGambit(row.gambit);
                rows.add(entry);
            }
            result["master"] = PGambits->MasterOn();
            result["rows"]   = rows;
            return result;
        };
        // Every edit saves the set (cardian_gambits)
        lua["CBaseEntity"]["cardianGambitToggle"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const bool on) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
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
        lua["CBaseEntity"]["cardianGambitMove"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 from, const uint32 to) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
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
        lua["CBaseEntity"]["cardianGambitDelete"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
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
        lua["CBaseEntity"]["cardianGambitInsert"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const std::string& spec) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
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
            if (const auto why = cardian::engage::pairingError(*gambit); !why.empty())
            {
                return std::string(why);
            }
            if (!PGambits->Insert(index, std::move(*gambit)))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitReplace"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint32 index, const std::string& spec) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
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
            if (const auto why = cardian::engage::pairingError(*gambit); !why.empty())
            {
                return std::string(why);
            }
            if (!PGambits->Replace(index, std::move(*gambit)))
            {
                return "no such row";
            }
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitVocab"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            auto* PPawn = commandPair(PLuaBaseEntity, name).second;
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
                    entry["mp"]      = e.mp;
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
        lua["CBaseEntity"]["cardianGambitMaster"] = [commandPair, gambitsOf](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            auto* PPawn    = commandPair(PLuaBaseEntity, name).second;
            auto* PGambits = gambitsOf(PPawn);
            if (PGambits == nullptr)
            {
                return "no such cardian";
            }
            PGambits->SetMaster(on);
            pawn::saveGambits(PPawn);
            return "";
        };
        lua["CBaseEntity"]["cardianGambitReset"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            pawn::forgetGambits(PPawn);
            return pawn::reloadBrain(PPawn) ? "" : "no such cardian";
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

        // The party strategy channel: orders live on the player and every
        // cardian of theirs, and every wild cardian in their party, follows them
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
            // The stake (RESEARCH §12.16): set or not, and where
            const auto stake     = pawn::stakeOf(PChar->id);
            result["staked"]     = stake.has_value();
            result["stake_zone"] = stake.has_value() ? static_cast<uint16>(stake->zone) : 0;
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
        lua["CBaseEntity"]["cardianWait"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const bool on) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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
                pawn::orderTravelByName(name, static_cast<uint16>(PChar->getZone()), PChar->id);
            }
            else
            {
                ShowInfoFmt("pawn: {} follows (ordered)", PPawn->getName());
            }
            return "";
        };

        lua["CBaseEntity"]["cardianWaiting"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            const auto* PController   = PPawn != nullptr ? dynamic_cast<const CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr && PController->IsWaiting();
        };

        // Her rest as the roster line carries it: the percentage her rest
        // order runs to (0: none), whether she kneels, Healing's ticks so
        // far, and milliseconds to the next tick and between ticks
        lua["CBaseEntity"]["cardianRestState"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            const auto* PController   = PPawn != nullptr ? dynamic_cast<const CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            if (PController == nullptr)
            {
                return sol::lua_nil;
            }
            const auto clock = PController->RestNow();
            auto       table = ::lua.create_table();
            table["percent"]  = PController->RestOrderPercent();
            table["down"]     = clock.down;
            table["ticks"]    = clock.ticks;
            table["next"]     = static_cast<int>(std::max(0.0, clock.next) * 1000.0);
            table["interval"] = static_cast<int>(clock.interval * 1000.0);
            return table;
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
        // The stake (RESEARCH §12.16): set or move it here, facing his way;
        // clear it. The chord's left arm and !cardian stake are this path
        lua["CBaseEntity"]["cardianStake"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return pawn::setStake(PChar);
        };
        lua["CBaseEntity"]["cardianStakeClear"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr)
            {
                return "no character";
            }
            return pawn::clearStake(PChar->id, "cleared") ? "" : "no stake";
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

        // Sent home alone, to the ordering player's home point, she waits
        // there as a warp leaves her (the user, 2026-09-14)
        lua["CBaseEntity"]["cardianHomePoint"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            if (!pawn::homePoint(PPawn, PChar))
            {
                return "not KO'd";
            }
            if (auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController()); PController != nullptr)
            {
                PController->SetWaiting(true, true, "waits at her home point");
            }
            return "";
        };

        // What she cannot do yet and for how long: the seconds left on
        // every spell and ability still on recast, keyed the way the
        // vocabulary keys them so a command list can label its own rows.
        // Only what is actually waiting is sent; the rest are ready.
        lua["CBaseEntity"]["cardianRecasts"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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
                for (auto* PAbility : pawn::abilitiesFor(PPawn))
                {
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
            return table;
        };

        // Her experience on her main job, and what the next level costs:
        // the character's own screens show it, and there is no upstream
        // getter for either
        lua["CBaseEntity"]["cardianExp"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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

        // The view origin (ROADMAP C, pawn/view.h): the player's client is
        // looking through this cardian, so the world around her must reach
        // him. "" looks through nobody again; a number is a target index in
        // his zone, any entity, for a GM (a player's eye through any mob
        // would be a wallhack).
        lua["CBaseEntity"]["cardianView"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            auto* PChar = dynamic_cast<CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            if (PChar == nullptr || PChar->loc.zone == nullptr)
            {
                return "not in a zone";
            }
            if (name.empty())
            {
                cardian::view::clear(PChar);
                return "";
            }
            CBaseEntity* PTarget = nullptr;
            if (std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c); }))
            {
                if (PChar->m_GMlevel == 0)
                {
                    return "a cardian's name";
                }
                PTarget = PChar->loc.zone->GetEntity(static_cast<uint16>(std::stoul(name)), TYPE_PC | TYPE_MOB | TYPE_NPC);
            }
            else
            {
                PTarget = commandPair(PLuaBaseEntity, name).second;
            }
            if (PTarget == nullptr || PTarget->loc.zone != PChar->loc.zone)
            {
                return "no such entity here";
            }
            cardian::view::set(PChar, PTarget);
            ShowInfoFmt("pawn: {} looks through {}", PChar->getName(), PTarget->getName());
            return "";
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

        // A walk order (pawn.h): a point in her zone, or none. The point is
        // the player's ring, which is its own thing on his client (no mesh
        // there): it is slid along the mesh from the last point toward the
        // one asked -- a wall or a ledge stops it, so it never leaves the
        // floor she can walk -- and its height is the mesh's. Answers the
        // error, then the point as taken (x, y, z), for the ring to follow
        lua["CBaseEntity"]["cardianWalk"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, sol::optional<float> x, sol::optional<float> y, sol::optional<float> z) -> std::tuple<std::string, float, float, float>
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return { "no such cardian", 0.f, 0.f, 0.f };
            }
            if (!x.has_value() || !y.has_value() || !z.has_value())
            {
                // A composed maneuver's route is its order's, not the ring's: the ring
                // going (the camera home) leaves it for her to walk at the release
                const auto* PController = dynamic_cast<const CPawnController*>(PPawn->PAI->GetController());
                if (PController == nullptr || !PController->ManeuverComposed())
                {
                    pawn::clearWalkOrder(PPawn->id);
                }
                return { "", 0.f, 0.f, 0.f };
            }
            if (PPawn->loc.zone == nullptr || PChar->loc.zone != PPawn->loc.zone)
            {
                return { "not in your zone", 0.f, 0.f, 0.f };
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            // Composed, her maneuver's way is set: a point still in flight from the ring moves nothing
            if (PController != nullptr && PController->ManeuverComposed())
            {
                const auto at = pawn::walkOrderOf(PPawn->id).value_or(PPawn->loc.p);
                return { "", at.x, at.y, at.z };
            }
            position_t point{ *x, *y, *z, 0, 0 };
            if (auto* PMesh = PPawn->loc.zone->navMesh(); PMesh != nullptr)
            {
                const auto from = pawn::walkOrderOf(PPawn->id).value_or(PPawn->loc.p);
                if (const auto slid = PMesh->findFurthestValidPoint(from, point); slid.has_value())
                {
                    point = *slid;
                }
                else
                {
                    point = from; // nowhere to slide from: the ring stays where it was
                }
                PMesh->snapToValidPosition(point); // the surface's own height
            }
            // Held, in a maneuver, the ring lays a route (docs/maneuvers.md)
            const bool laying = PController != nullptr && PController->InManeuver() && cardian::pause::isHeld();
            pawn::setWalkOrder(PPawn->id, point, PChar->id, laying);
            return { "", point.x, point.y, point.z };
        };

        // The command window: one action now, on a target index in the
        // zone (0 = herself)
        lua["CBaseEntity"]["cardianDo"] = [commandPair, managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& key, const uint16 targid) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            // Her items are hers to use only when she is his: use is a manage verb
            if (key.starts_with("item:") && managedPair(PLuaBaseEntity, name).second == nullptr)
            {
                return name + " is not yours to manage";
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
                ShowInfoFmt("pawn: {} is ordered {} on {} by {}", PPawn->getName(), key, PTarget->getName(), PChar->getName());
            }
            return err;
        };

        // A maneuver (docs/maneuvers.md, pawn_controller.h): begins one on
        // her; "off" ends it; "move", "movewait" and "rest:<n>" are its
        // orders that are no action. Answers "" or why not
        lua["CBaseEntity"]["cardianManeuver"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const std::string& what) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            if (PController == nullptr)
            {
                return "no controller";
            }
            if (what == "off")
            {
                if (!PController->InManeuver())
                {
                    return "no maneuver";
                }
                PController->EndManeuver(fmt::format("{} cancels the maneuver", PChar->getName()));
                return "";
            }
            if (what == "move" || what == "movewait")
            {
                return PController->ComposeMove(what == "movewait");
            }
            if (int percent = 0; std::sscanf(what.c_str(), "rest:%d", &percent) == 1)
            {
                return PController->ComposeRest(percent);
            }
            return PController->BeginManeuver(PChar);
        };
        // A composed maneuver of his waiting on her (a pause queues one per
        // cardian): told to an addon that has just bound, which starts empty
        lua["CBaseEntity"]["cardianComposed"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> bool
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            const auto* PController   = PPawn != nullptr ? dynamic_cast<const CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr && PChar != nullptr && PController->ManeuverComposed() && PController->ManeuverBy() == PChar->id;
        };
        // The cardian this player has a maneuver on, her name, or ""
        lua["CBaseEntity"]["cardianManeuverOf"] = [](CLuaBaseEntity* PLuaBaseEntity) -> std::string
        {
            const auto* PChar       = dynamic_cast<const CCharEntity*>(PLuaBaseEntity->GetBaseEntity());
            const auto* PPawn       = PChar != nullptr ? zoneutils::GetChar(pawn::maneuverOf(PChar->id)) : nullptr;
            const auto* PController = PPawn != nullptr && PPawn->PAI != nullptr ? dynamic_cast<const CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr && PController->InManeuver() ? PPawn->getName() : std::string();
        };

        // The command window's queue line: what she has waiting ("" with none),
        // and the player taking it back
        lua["CBaseEntity"]["cardianQueued"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            auto* PController         = PPawn != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
            return PController != nullptr ? PController->QueuedOrderLine() : "";
        };
        lua["CBaseEntity"]["cardianCancel"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            if (PPawn == nullptr)
            {
                return "no such cardian";
            }
            auto* PController = dynamic_cast<CPawnController*>(PPawn->PAI->GetController());
            return PController != nullptr && PController->CancelQueuedOrder() ? "" : "nothing queued";
        };

        lua["CBaseEntity"]["cardianRescue"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> std::string
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::rescue(PChar, PPawn) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianUse"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint8 location) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::useItem(PPawn, slot, location) : "no such cardian";
        };

        lua["CBaseEntity"]["cardianDrop"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 slot, const uint32 qty, const uint8 location) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::dropItem(PPawn, slot, qty, location) : "no such cardian";
        };

        // Merge and compact one of her containers (the inventory or a bag)
        lua["CBaseEntity"]["cardianSort"] = [managedPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name, const uint8 location) -> std::string
        {
            const auto [PChar, PPawn] = managedPair(PLuaBaseEntity, name);
            return PPawn != nullptr ? pawn::items::sortBag(PPawn, location) : "no such cardian";
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

            // Refused whole while held, as the use would be: not half of it, the transfer
            if (cardian::pause::isHeld())
            {
                return "not while paused";
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
        lua["CBaseEntity"]["cardianCombatStats"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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
        lua["CBaseEntity"]["cardianProfile"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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

        lua["CBaseEntity"]["cardianGear"] = [commandPair](CLuaBaseEntity* PLuaBaseEntity, const std::string& name) -> sol::object
        {
            const auto [PChar, PPawn] = commandPair(PLuaBaseEntity, name);
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
