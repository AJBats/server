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

#include "live_controller.h"

#include "engage_math.h"
#include "pawn.h"
#include "pawn_danger.h"
#include "spell_bank.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/utils.h"

#include "ability.h"
#include "ai/ai_container.h"
#include "ai/states/ability_state.h"
#include "ai/states/item_state.h"
#include "ai/states/magic_state.h"
#include "ai/states/range_state.h"
#include "ai/states/weaponskill_state.h"
#include "entities/char_entity.h"
#include "entities/mob_entity.h"
#include "items/item_weapon.h"
#include "lua/luautils.h"
#include "packets/basic.h"
#include "party.h"
#include "recast_container.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"
#include "utils/charutils.h"
#include "weapon_skill.h"
#include "zone.h"

#include <algorithm>
#include <memory>
#include <typeinfo>
#include <vector>

namespace
{
    // How long he stands still before his rows cast or shoot: a step he is
    // taking shows in his client's next position packet well within it
    constexpr auto kStillFor = 600ms;
    // The quiet after the game refused an action his rows started
    constexpr auto kQuietFor = 8s;
    // How often his Attack rows look for a fight
    constexpr auto kDoorEvery = 1s;
    // How far he may engage from: upstream's reach (CPlayerController::Engage),
    // a little inside it
    constexpr float kEngageReach = 29.5f;

    auto leaderOf(CCharEntity* PChar) -> CCharEntity*
    {
        return PChar->PParty != nullptr ? dynamic_cast<CCharEntity*>(PChar->PParty->GetLeader()) : nullptr;
    }

    auto words(const std::string& raw) -> std::string
    {
        std::string out = raw;
        std::ranges::replace(out, '_', ' ');
        return out;
    }

    // A character his own client drives, as his gambit engine sees him
    // (gambit_host.h): his rows are orders, his hands act through the doors
    // his client's packets reach, and an action is tried only when the game
    // would take it -- in reach, off its recast, nothing keeping him from it
    // -- so a row that cannot act now passes to the next without a word to
    // him. Whatever the game refuses all the same it says to him itself,
    // and his rows go quiet a while (CLiveController::Refused)
    class LiveHost final : public pawn::GambitHost
    {
    public:
        LiveHost(CLiveController& controller, CCharEntity* PChar)
        : m_controller(controller)
        , m_PChar(PChar)
        {
        }

        auto OwnClient() const -> bool override
        {
            return true;
        }

        auto Cast(const EntityId target, const SpellID spell) -> bool override
        {
            auto* PSpell = spell::GetSpell(spell);
            if (PSpell == nullptr)
            {
                return false;
            }
            const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(m_PChar) : target;
            auto*          PTarget    = castTarget.resolve<CBattleEntity>();
            if (PTarget == nullptr || PTarget->loc.zone != m_PChar->loc.zone || !m_controller.StandingStill() ||
                !CastWouldTake(PSpell, castTarget, PTarget) || pawn::partyAlreadyCasting(m_PChar, PSpell, PTarget))
            {
                return false;
            }
            return Started(m_PChar->PAI->Cast(castTarget, spell), "casts", PSpell->getName(), PTarget);
        }

        auto CastAssigned(const EntityId /*target*/, const SpellID /*spell*/) -> bool override
        {
            return false; // the conveyor assigns a cardian's casts, never his
        }

        auto Ability(const EntityId target, const uint16 ability) -> bool override
        {
            auto* PAbility = ability::GetAbility(ability);
            auto* PTarget  = target.resolve<CBattleEntity>();
            if (PAbility == nullptr || PTarget == nullptr || PTarget->loc.zone != m_PChar->loc.zone || !AbilityWouldTake(PAbility, PTarget))
            {
                return false;
            }
            return Started(m_PChar->PAI->Ability(target, ability), "uses", PAbility->getName(), PTarget);
        }

        auto WeaponSkill(const EntityId target, const uint16 skill) -> bool override
        {
            auto* PSkill  = battleutils::GetWeaponSkill(skill);
            auto* PTarget = target.resolve<CBattleEntity>();
            if (PSkill == nullptr || PTarget == nullptr || Amnesic())
            {
                return false;
            }
            if (PTarget != m_PChar)
            {
                const auto type   = static_cast<xi::SkillType>(PSkill->getType());
                const bool ranged = type == xi::SkillType::Archery || type == xi::SkillType::Marksmanship;
                const auto reach  = ranged ? m_PChar->GetRangedAttackRange() : m_PChar->GetMeleeRange(PTarget);
                // He faces his target himself: the game refuses a weapon
                // skill he is turned away from
                if (distance(m_PChar->loc.p, PTarget->loc.p) > reach || !facing(m_PChar->loc.p, PTarget->loc.p, 64))
                {
                    return false;
                }
            }
            return Started(m_PChar->PAI->WeaponSkill(target, skill), "uses", PSkill->getName(), PTarget);
        }

        auto RangedAttack(const EntityId target) -> bool override
        {
            auto*       PTarget = target.resolve<CBattleEntity>();
            const auto* PRanged = dynamic_cast<CItemWeapon*>(m_PChar->getEquip(SLOT_RANGED));
            const auto* PAmmo   = dynamic_cast<CItemWeapon*>(m_PChar->getEquip(SLOT_AMMO));
            const bool  armed   = (PRanged != nullptr && PRanged->isType(ITEM_WEAPON)) || (PAmmo != nullptr && PAmmo->isThrowing());
            if (PTarget == nullptr || !armed || !m_controller.StandingStill() || distance(m_PChar->loc.p, PTarget->loc.p) > m_PChar->GetRangedAttackRange())
            {
                return false;
            }
            return Started(m_PChar->PAI->RangedAttack(target), "shoots", "", PTarget);
        }

        // He has no behaviour layer, no role and no tactician
        void ClearGambitBehaviors() override
        {
        }
        void SetGambitBehavior(const uint16 /*behavior*/, const uint16 /*arg*/) override
        {
        }
        auto HoldsRole(const pawn::Role /*role*/) const -> bool override
        {
            return false;
        }
        // His own commands wait in his client, or in the pause's gate while
        // held, when his rows do not run
        auto HasQueuedOrder() const -> bool override
        {
            return false;
        }
        auto IsRetreating() const -> bool override
        {
            return false;
        }
        // His rest is his: the controller runs nothing while he kneels
        auto RestAllowsAction() const -> bool override
        {
            return !m_PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
        }
        auto Acting() const -> bool override
        {
            return m_PChar->PAI->IsCurrentState<CMagicState>() || m_PChar->PAI->IsCurrentState<CWeaponSkillState>() ||
                   m_PChar->PAI->IsCurrentState<CAbilityState>() || m_PChar->PAI->IsCurrentState<CRangeState>() ||
                   m_PChar->PAI->IsCurrentState<CItemState>();
        }
        auto CanAct() -> bool override
        {
            return m_controller.canAct();
        }
        auto PartyPosition() const -> uint8 override
        {
            return 0;
        }
        auto TacticianRuns() const -> bool override
        {
            return false;
        }
        auto IsWorld() const -> bool override
        {
            return false;
        }
        // `Ally: the player` is himself
        auto GetLivePlayer() const -> CCharEntity* override
        {
            return m_PChar;
        }
        auto GetTopEnmity() const -> CBattleEntity* override
        {
            return pawn::topEnmityOf(m_PChar);
        }
        auto FoeOfKind(const cardian::engage::Finder finder, CBattleEntity* PFoe) const -> bool override
        {
            return PFoe != nullptr && PFoe->objtype == TYPE_MOB && cardian::engage::accepts(finder, pawn::foeFacts(m_PChar, PFoe, leaderOf(m_PChar)));
        }
        // His battle target is his fight: there is no other he attends
        auto PartyFightTarget() const -> CBattleEntity* override
        {
            return nullptr;
        }

    private:
        auto Amnesic() const -> bool
        {
            return m_PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Amnesia);
        }

        // The game would take this cast now: the checks the magic state
        // makes as it starts (CMagicState::init, CanCastSpell, HasCost), in
        // its order, short of moving him. The spell's own Lua check runs on a
        // copy, as the state's does, so nothing it sets lands on the table
        auto CastWouldTake(CSpell* PSpell, const EntityId castTarget, CBattleEntity* PTarget) -> bool
        {
            std::unique_ptr<CBasicPacket> unused;
            if (m_PChar->IsValidTarget(castTarget, PSpell->getValidTarget(), unused) == nullptr || !m_PChar->CanUseSpell(PSpell) ||
                !m_PChar->loc.zone->CanUseMisc(PSpell->getZoneMisc()) ||
                m_PChar->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }))
            {
                return false;
            }
            if (const auto* POmerta = m_PChar->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Omerta);
                POmerta != nullptr && ((1 << (PSpell->getSpellGroup() - 1)) & POmerta->GetPower()) != 0)
            {
                return false;
            }
            const bool paid = PSpell->getSpellGroup() == SPELLGROUP_NINJUTSU ? battleutils::HasNinjaTool(m_PChar, PSpell, false)
                                                                             : battleutils::CanAffordSpell(m_PChar, PSpell);
            if (!paid || PTarget->IsNameHidden())
            {
                return false;
            }
            if (PTarget != m_PChar &&
                (distance(m_PChar->loc.p, PTarget->loc.p) > 40.0f || distance(m_PChar->loc.p, PTarget->loc.p) > pawn::tactics::bank::castRange(m_PChar, PSpell, PTarget) ||
                 (m_PChar->loc.zone->CanUseMisc(xi::ZoneMisc::LosPlayerBlock) && !m_PChar->CanSeeTarget(PTarget))))
            {
                return false;
            }
            auto copy = PSpell->clone();
            return luautils::OnMagicCastingCheck(m_PChar, PTarget, copy.get()) == 0;
        }

        // The game would let this ability go off now: the checks the
        // ability state makes for a character once it starts
        // (CAbilityState::CanUseAbility) -- which it makes a tick after
        // saying it started, so a refusal never reaches the engine. Its Lua
        // check runs on a copy, as the state's does
        auto AbilityWouldTake(CAbility* PAbility, CBattleEntity* PTarget) -> bool
        {
            const auto* PImpairment = m_PChar->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Impairment);
            if (m_PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime()) || Amnesic() ||
                (PImpairment != nullptr && (PImpairment->GetPower() == 0x01 || PImpairment->GetPower() == 0x03)) ||
                !charutils::hasAbility(m_PChar, PAbility->getID()))
            {
                return false;
            }
            std::unique_ptr<CBasicPacket> unused;
            if (m_PChar->IsValidTarget(PTarget->targid, PAbility->getValidTarget(), unused) == nullptr)
            {
                return false;
            }
            if (PTarget != m_PChar &&
                (distance(m_PChar->loc.p, PTarget->loc.p) > PAbility->getRange() + m_PChar->modelHitboxSize + PTarget->modelHitboxSize ||
                 (m_PChar->loc.zone->CanUseMisc(xi::ZoneMisc::LosPlayerBlock) && !m_PChar->CanSeeTarget(PTarget))))
            {
                return false;
            }
            CAbility     copy(*PAbility);
            CBaseEntity* PMsgTarget = m_PChar;
            return luautils::OnAbilityCheck(m_PChar, PTarget, &copy, &PMsgTarget) == 0;
        }

        // An action the game took is said in the map log; one it refused
        // quiets his rows
        auto Started(const bool started, const std::string_view verb, const std::string& what, const CBattleEntity* PTarget) -> bool
        {
            const auto action = what.empty() ? std::string(verb) : fmt::format("{} {}", verb, words(what));
            if (!started)
            {
                m_controller.Refused(action);
                return false;
            }
            ShowInfoFmt("own gambits: {} {} on {}", m_PChar->getName(), action, PTarget != nullptr ? PTarget->getName() : "-");
            return true;
        }

        CLiveController& m_controller;
        CCharEntity*     m_PChar;
    };
} // namespace

CLiveController::CLiveController(CCharEntity* PChar)
: CPlayerController(PChar)
, m_Host(std::make_unique<LiveHost>(*this, PChar))
, m_Gambits(std::make_unique<pawn::CGambits>(PChar, m_Host.get()))
{
    // His own set, else nothing: off and empty until he builds one (the
    // engine starts a played character's switch off)
    if (const auto master = pawn::loadGambitSet(PChar, pawn::kOwnClientSet, *m_Gambits); master.has_value())
    {
        m_Gambits->SetMaster(*master);
    }
}

CLiveController::~CLiveController() = default;

auto CLiveController::Gambits() -> pawn::CGambits&
{
    return *m_Gambits;
}

auto CLiveController::StandingStill() const -> bool
{
    return timer::now() - m_StillSince >= kStillFor;
}

void CLiveController::Refused(const std::string_view what)
{
    m_QuietUntil = timer::now() + kQuietFor;
    ShowInfoFmt("own gambits: the game refused {}'s {}; his rows wait {} s", POwner->getName(), what, timer::count_seconds(kQuietFor));
}

void CLiveController::LeavingByHand()
{
    if (auto* PMob = POwner->GetBattleTarget(); PMob != nullptr && POwner->PAI->IsEngaged() && !PMob->isDead())
    {
        m_LeftAlive = PMob->entityId();
    }
}

void CLiveController::InstallOn(CCharEntity* PChar)
{
    // Exactly upstream's own controller: never a cardian's, nor this one
    // again, nor whatever a charm put on him while it lasts
    if (auto* PController = PChar != nullptr && PChar->PAI != nullptr ? PChar->PAI->GetController() : nullptr;
        PChar != nullptr && PChar->PSession != nullptr && !pawn::isPawn(PChar) && PController != nullptr && typeid(*PController) == typeid(CPlayerController))
    {
        PChar->PAI->SetController(std::make_unique<CLiveController>(PChar));
    }
}

// He can act as the game judges it: not asleep, stunned or otherwise held
// (an inactive state), and past the wait after his last cast, which the game
// refuses every action through
auto CLiveController::Ready() -> bool
{
    const auto* PChar = static_cast<const CCharEntity*>(POwner);
    return PChar->PSession != nullptr && !PChar->isDead() && !PChar->isInEvent() && PChar->status == xi::Status::Normal && !PChar->isMounted() &&
           !PChar->inMogHouse() && !PChar->requestedZoneChange && !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) &&
           PChar->PAI->CanChangeState() && canAct() && timer::now() >= m_QuietUntil;
}

auto CLiveController::Tick(const timer::time_point tick) -> Task<void>
{
    co_await CPlayerController::Tick(tick);

    // His stillness, every tick, off the position his client last reported
    const auto& at = POwner->loc.p;
    if (at.x != m_LastPos.x || at.y != m_LastPos.y || at.z != m_LastPos.z)
    {
        m_LastPos    = at;
        m_StillSince = timer::now();
    }
    WatchLeftAlive();

    if (!m_Gambits->MasterOn() || !Ready())
    {
        co_return;
    }

    m_Gambits->Tick(tick, POwner->PAI->IsEngaged());

    if (tick >= m_NextDoor)
    {
        m_NextDoor = tick + kDoorEvery;
        EngageDoor();
    }
    co_return;
}

void CLiveController::WatchLeftAlive()
{
    if (m_LeftAlive.isSet())
    {
        const auto* PMob = m_LeftAlive.resolve<CBattleEntity>();
        if (PMob == nullptr || PMob->isDead())
        {
            m_LeftAlive.clean();
        }
    }
}

void CLiveController::EngageDoor()
{
    auto* PChar = static_cast<CCharEntity*>(POwner);
    // His target is his: a fight is taken only while he is out of one
    if (PChar->PAI->IsEngaged() || !PChar->PAI->CanChangeState())
    {
        return;
    }
    // Upstream's re-engage wait, as CPlayerController::Engage judges it:
    // the mob he last engaged waits his weapon's delay, any other the
    // switch delay. A foe still inside its wait is left for a later look,
    // so the game is never asked early and never says "wait longer"
    const auto lastEngaged = static_cast<uint32>(PChar->GetLocalVar("cardianLastEngaged"));
    const auto switchWait  = std::chrono::milliseconds(static_cast<int64>(settings::get<float>("cardian.REENGAGE_SWITCH_DELAY") * 1000.0f));
    const auto waited      = [&](const CBattleEntity* PFoe)
    {
        const auto wait = PFoe->id == lastEngaged ? std::chrono::milliseconds(PChar->GetWeaponDelay(false)) : switchWait;
        return m_lastAttackTime + wait < timer::now();
    };
    const auto rows = m_Gambits->EngageRows();
    if (rows.empty())
    {
        return;
    }

    // The foes around him, each within his engage reach: his leader's
    // engaged target, his allies' fights, and a mob that has come for him or
    // one of his party. The mob he left by his own hand is his to leave: a
    // disengage of his own is not undone by his rows
    auto*                       PLeader = leaderOf(PChar);
    std::vector<CBattleEntity*> foes;
    const auto                  add = [&](CBattleEntity* PFoe)
    {
        if (PFoe != nullptr && PFoe->objtype == TYPE_MOB && !PFoe->isDead() && PFoe->loc.zone == PChar->loc.zone && !(m_LeftAlive == PFoe) &&
            distance(PChar->loc.p, PFoe->loc.p) < kEngageReach && waited(PFoe) && std::ranges::find(foes, PFoe) == foes.end())
        {
            foes.push_back(PFoe);
        }
    };
    if (PLeader != nullptr && PLeader->PAI->IsEngaged())
    {
        add(PLeader->GetBattleTarget());
    }
    if (PChar->PParty != nullptr)
    {
        for (auto* PMember : PChar->PParty->members)
        {
            auto* PAlly = dynamic_cast<CCharEntity*>(PMember);
            if (PAlly != nullptr && PAlly != PChar && PAlly->loc.zone == PChar->loc.zone && PAlly->PAI->IsEngaged())
            {
                add(PAlly->GetBattleTarget());
            }
        }
    }
    pawn::forEachMobNear(pawn::entitiesAround(PChar), PChar->loc.p, kEngageReach, [&](CMobEntity* PMob)
                         {
                             if (PMob->PAI->IsEngaged() && pawn::foeFacts(PChar, PMob, PLeader).onParty)
                             {
                                 add(PMob);
                             }
                         });
    if (foes.empty())
    {
        return;
    }

    std::vector<cardian::engage::Foe> facts;
    facts.reserve(foes.size());
    for (auto* PFoe : foes)
    {
        facts.push_back(pawn::foeFacts(PChar, PFoe, PLeader));
    }
    std::vector<cardian::engage::Row> view;
    view.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        view.push_back({ i + 1, rows[i].gambit->target_selector, true });
    }
    const auto pick = cardian::engage::chooseRow(m_Gambits->MasterOn(), false, view, facts, [&](const std::size_t row, const std::size_t foe)
                                                 {
                                                     return m_Gambits->EngageConditionsHold(*rows[row].gambit, foes[foe]);
                                                 });
    if (!pick.has_value())
    {
        return;
    }
    auto* PFoe = foes[pick->foe];
    if (!PChar->PAI->Engage(PFoe->entityId()))
    {
        Refused("engage");
        return;
    }
    ShowInfoFmt("own gambits: {} engages {} (his row {})", PChar->getName(), PFoe->getName(), rows[pick->row - 1].index);
}
