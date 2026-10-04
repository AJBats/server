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
#include "reengage.h"
#include "rest_policy.h"
#include "spell_bank.h"
#include "tactics.h"
#include "pause/input_gate.h"

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
    // (gambit_host.h): his rows run as a cardian's, his hands act through the doors
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

        // A row's own cast, where no tactician watches his party: another
        // member's cast of it already under way passes it by
        auto Cast(const EntityId target, const SpellID spell) -> bool override
        {
            return CastIfTaken(target, spell, true);
        }

        auto CastAssigned(const EntityId target, const SpellID spell) -> bool override
        {
            return CastIfTaken(target, spell, false);
        }

        auto FreeToCast(CSpell* PSpell, CBattleEntity* PTarget) -> bool override
        {
            if (PSpell == nullptr || PTarget == nullptr)
            {
                return false;
            }
            auto* PCastTarget = PSpell->getValidTarget() == TARGET_SELF ? m_PChar : PTarget;
            return m_controller.Ready() && m_controller.StandingStill() && CastWouldTake(PSpell, EntityId(PCastTarget), PCastTarget);
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
        // Sneak Attack and Boost before a weapon skill are a cardian
        // controller's own sequence (the opener, then the weapon skill held
        // until it lands): not his yet
        auto SneakAttackNow(const CBattleEntity* /*PTarget*/) -> bool override
        {
            return false;
        }
        // His own command waiting in the pause's gate: it goes first at the
        // release
        auto HasQueuedOrder() const -> bool override
        {
            return cardian::pause::input::queued(m_PChar->id).has_value();
        }
        auto IsRetreating() const -> bool override
        {
            return false;
        }
        // His kneel, on a cardian's rest lifecycle (CLiveController::RestAllowsAction)
        auto RestAllowsAction() const -> bool override
        {
            return m_controller.RestAllowsAction();
        }
        auto RestReadyIn(const double now) const -> double override
        {
            return m_controller.RestReadyIn(now);
        }
        auto RestInterruptionCost() const -> double override
        {
            return pawn::tactics::restInterruptionCost(m_PChar);
        }
        auto StandsToCast() const -> bool override
        {
            return m_controller.StandingStill() && m_controller.RowsMayAct();
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
        // His think's place after his party's cardians', so none thinks in
        // step with him
        auto PartyPosition() const -> uint8 override
        {
            uint8 cardians = 0;
            if (m_PChar->PParty != nullptr)
            {
                for (const auto* PMember : m_PChar->PParty->members)
                {
                    if (const auto* PChar = dynamic_cast<const CCharEntity*>(PMember); PChar != nullptr && pawn::isPawn(PChar))
                    {
                        ++cardians;
                    }
                }
            }
            return cardians;
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
        // The one whose fights are the party's: his party's leader
        auto Anchor() const -> CCharEntity* override
        {
            return leaderOf(m_PChar);
        }
        // A cardian's door holds off a target it was refused; his door's
        // refusals quiet his rows instead (Refused), and the mob he left by
        // hand is passed by his door alone (EngageDoor): he may still
        // attend it, a mage's spells on it
        auto HoldingOff(const CBattleEntity* /*PFoe*/) const -> bool override
        {
            return false;
        }
        // His own command engaged him on it
        auto OrderedOnto(const CBattleEntity* PTarget) const -> bool override
        {
            return PTarget != nullptr && m_PChar->PAI->IsEngaged() && m_PChar->GetBattleTarget() == PTarget;
        }
        auto PartyFightTarget() const -> CBattleEntity* override
        {
            return m_controller.AttendedFight();
        }

    private:
        auto Amnesic() const -> bool
        {
            return m_PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Amnesia);
        }

        // A cast started only where the game would take it from where he
        // stands; `party` also passes it by while another member casts it
        auto CastIfTaken(const EntityId target, const SpellID spell, const bool party) -> bool
        {
            auto* PSpell = spell::GetSpell(spell);
            if (PSpell == nullptr)
            {
                return false;
            }
            const EntityId castTarget = PSpell->getValidTarget() == TARGET_SELF ? EntityId(m_PChar) : target;
            auto*          PTarget    = castTarget.resolve<CBattleEntity>();
            if (PTarget == nullptr || PTarget->loc.zone != m_PChar->loc.zone || !m_controller.StandingStill() ||
                !CastWouldTake(PSpell, castTarget, PTarget) || (party && pawn::partyAlreadyCasting(m_PChar, PSpell, PTarget)))
            {
                return false;
            }
            return Started(m_PChar->PAI->Cast(castTarget, spell), "casts", PSpell->getName(), PTarget);
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
            if (m_PChar->PRecastContainer->HasRecast(RECAST_ABILITY, PAbility->getRecastId(), PAbility->getRecastTime()) || pawn::abilitiesShutOut(m_PChar) ||
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

auto CLiveController::RowsMayAct() const -> bool
{
    const auto* PChar = static_cast<const CCharEntity*>(POwner);
    return PChar->PSession != nullptr && !PChar->isDead() && !PChar->isInEvent() && PChar->status == xi::Status::Normal && !PChar->isMounted() &&
           !PChar->inMogHouse() && !PChar->requestedZoneChange && !PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Leavegame) &&
           timer::now() >= m_QuietUntil;
}

// He can act as the game judges it: not asleep, stunned or otherwise held
// (an inactive state), and past the wait after his last cast, which the game
// refuses every action through
auto CLiveController::Ready() -> bool
{
    return RowsMayAct() && !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing) && POwner->PAI->CanChangeState() && canAct();
}

auto CLiveController::Tick(const timer::time_point tick) -> Task<void>
{
    co_await CPlayerController::Tick(tick);

    // His party's tactician, advanced once a tick by whoever asks first, as
    // every cardian's tick asks: with no cardian beside him it is his alone
    pawn::tactics::tick(static_cast<CCharEntity*>(POwner), tick);

    // His kneel, every tick, as the rest lifecycle reads it; and first aid's
    // call to stand
    m_Rest.observe(POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing), pawn::tactics::restSeconds(timer::now()));
    WakeForFirstAid();

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
        m_Attended.clean();
        co_return;
    }

    // Out of a fight himself, the party's fight around him is one he
    // attends as a cardian mage does (CGambits::AttendsFight): his rows read
    // it as "the mob", and are in the fight once it is engaged
    const bool engaged = POwner->PAI->IsEngaged();
    m_Attended.clean();
    if (!engaged)
    {
        auto* PChar = static_cast<CCharEntity*>(POwner);
        if (auto* PFight = m_Gambits->PartyFightScan(leaderOf(PChar), PChar->loc.p).target; PFight != nullptr && m_Gambits->AttendsFight(PFight))
        {
            m_Attended = PFight->entityId();
        }
    }
    const auto* PAttended = AttendedFight();
    m_Gambits->Tick(tick, engaged || (PAttended != nullptr && PAttended->PAI->IsEngaged()));

    if (tick >= m_NextDoor)
    {
        m_NextDoor = tick + kDoorEvery;
        EngageDoor();
    }
    co_return;
}

auto CLiveController::RestAllowsAction() const -> bool
{
    return pawn::tactics::kneelAllowsAction(m_Rest, POwner);
}

auto CLiveController::RestReadyIn(const double now) const -> double
{
    return pawn::tactics::kneelReadyIn(m_Rest, POwner, now);
}

void CLiveController::WakeForFirstAid()
{
    // The emergency cure stands any kneeling caster the party picks for it,
    // as it stands a cardian (the user, 2026-10-03): the wake is about the
    // cure, not his rest. Never through his logout: its countdown kneels
    // him too, and standing would call it off
    auto* PChar = static_cast<CCharEntity*>(POwner);
    if (!m_Gambits->MasterOn() || PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Leavegame))
    {
        return;
    }
    if (const auto advice = pawn::tactics::restAdvice(PChar); advice.has_value() && advice->wake)
    {
        pawn::tactics::standFromKneel(m_Rest, PChar, advice->why);
    }
}

auto CLiveController::LeftByHand(const CBattleEntity* PFoe) const -> bool
{
    return PFoe != nullptr && m_LeftAlive == PFoe;
}

auto CLiveController::AttendedFight() const -> CBattleEntity*
{
    auto* PFight = m_Attended.isSet() ? m_Attended.resolve<CBattleEntity>() : nullptr;
    return PFight != nullptr && !PFight->isDead() ? PFight : nullptr;
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
    // The door's pick, a cardian's own (CGambits::EngageChoice): his Attack
    // rows top down over the party's foes around him -- of those he can
    // take now. What a cardian walks to, he takes only within upstream's
    // engage reach of where he stands; only once his re-engage wait is over,
    // as CPlayerController::Engage judges it (reengage.h), so the game is
    // never asked early and never says "wait longer"; and never the mob he
    // left by his own hand
    const auto takes = [&](CBattleEntity* PFoe)
    {
        return PFoe->loc.zone == PChar->loc.zone && distance(PChar->loc.p, PFoe->loc.p) < kEngageReach && !LeftByHand(PFoe) &&
               timer::now() >= cardian::reengage::playerReadyAt(PChar, PFoe, m_engageLockedUntil, m_lastAttackTime);
    };
    const auto pick = m_Gambits->EngageChoice(leaderOf(PChar), PChar->loc.p, takes);
    auto*      PFoe = pick.target;
    if (PFoe == nullptr)
    {
        return;
    }
    if (!PChar->PAI->Engage(PFoe->entityId()))
    {
        Refused("engage");
        return;
    }
    ShowInfoFmt("own gambits: {} engages {} ({})", PChar->getName(), PFoe->getName(), pick.why);
}
