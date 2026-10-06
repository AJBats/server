// Cardian: her food (RESEARCH §19). Her "Self -> Eat with the player" row
// eats her party role's food when the player has food on and she has none,
// looked at every 30 seconds and eaten at her first free moment between
// fights; a Healer whose food is a cookie eats it as she kneels for MP
// instead (RestTick); and a body of the world in the player's party is kept
// topped up, so she never runs out. What she eats is pawn/food.h's, the
// rules food_math.h's.
#include "food.h"
#include "party_roster.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "items/item.h"
#include "status_effect_container.h"
#include "utils/itemutils.h"

#include <fmt/format.h>

namespace
{
    auto fed(const CBattleEntity* PEntity) -> bool
    {
        return PEntity->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Food);
    }

    auto itemName(const uint16 itemId) -> std::string
    {
        const auto* PKind = xi::items::lookup(itemId);
        return PKind != nullptr ? PKind->getName() : fmt::format("item {}", itemId);
    }
} // namespace

auto CPawnController::EatsWithPlayer() const -> bool
{
    return Behavior(pawn::Behavior::EatWithPlayer).value_or(0) != 0;
}

void CPawnController::SayFood(const std::string& line)
{
    if (line == m_FoodSaid)
    {
        return;
    }
    m_FoodSaid = line;
    if (!line.empty())
    {
        ShowInfoFmt("pawn: {} {}", POwner->getName(), line);
    }
}

void CPawnController::FoodTick()
{
    auto* PChar   = static_cast<CCharEntity*>(POwner);
    auto* PPlayer = pawn::partyPlayer(PChar);
    if (PPlayer == nullptr || POwner->isDead())
    {
        m_FoodDue.reset();
        return;
    }

    const auto now = timer::now();
    if (now >= m_FoodLookAt)
    {
        m_FoodLookAt = now + cardian::food::kCheckEvery;
        // A body of the world in his party never runs out (the user,
        // 2026-10-05); a recruit's food, and his own cardians', is his
        if (pawn::seats::isWorlds(PChar->id))
        {
            pawn::food::topUp(PChar);
        }
        m_FoodDue.reset();
        if (m_Gambits->MasterOn() && EatsWithPlayer() && fed(PPlayer) && !fed(PChar))
        {
            const auto role = pawn::roster::roleOf(PChar);
            const auto pick = pawn::food::pickFor(PChar, role);
            if (!pick.has_value())
            {
                SayFood(fmt::format("has no food for her role ({}) to eat with the player", cardian::party::roleName(role)));
            }
            else if (pick->cookie)
            {
                SayFood(fmt::format("keeps her {} for her next kneel", itemName(pick->itemId)));
            }
            else
            {
                m_FoodDue = pick->itemId;
            }
        }
    }
    if (!m_FoodDue.has_value())
    {
        return;
    }

    // Due, she eats at her first moment between fights that she is free:
    // standing still (a step cuts an item's use short), up and done standing
    // from a kneel (the rest policy's word: a use asked sooner is refused),
    // not acting, nothing of his to carry out, no enchanted item on its way,
    // no rest order on
    const bool betweenFights = !POwner->PAI->IsEngaged() && !PPlayer->PAI->IsEngaged() && PartyFightTarget() == nullptr && m_Mode != Mode::Fight &&
                               m_Mode != Mode::Hold && m_Mode != Mode::Attend;
    const bool still = POwner->PAI->PathFind == nullptr || !POwner->PAI->PathFind->IsFollowingPath();
    const bool free  = still && !Acting() && ReadyToAct() && RestAllowsAction() && !InManeuver() && !HasQueuedOrder() && !HasPlayersOrder() &&
                      !m_Enchant.has_value() && !m_RestOrder.active() && m_Mode != Mode::Travel &&
                      !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    const cardian::food::WithPlayer moment{
        .rowOn         = m_Gambits->MasterOn() && EatsWithPlayer(),
        .playerFed     = fed(PPlayer),
        .selfFed       = fed(PChar),
        .betweenFights = betweenFights,
        .free          = free,
    };
    // the reason to eat has gone: his food wore off, hers came on, the row went off
    if (!moment.rowOn || !moment.playerFed || moment.selfFed)
    {
        m_FoodDue.reset();
        return;
    }
    if (!cardian::food::eatsWithPlayer(moment))
    {
        return;
    }
    const auto itemId = *m_FoodDue;
    m_FoodDue.reset();
    if (const auto status = pawn::food::eat(PChar, itemId); status != CL_S_OK)
    {
        SayFood(fmt::format("could not eat her {} with the player (outcome 0x{:04X})", itemName(itemId), status));
        return;
    }
    m_FoodSaid.clear();
    ShowInfoFmt("pawn: {} eats {} with the player", POwner->getName(), itemName(itemId));
}

auto CPawnController::EatCookieBeforeKneel(const bool aboutToKneel, const bool shortOfMp) -> bool
{
    if (!aboutToKneel || !shortOfMp)
    {
        return false;
    }
    auto*      PChar   = static_cast<CCharEntity*>(POwner);
    auto*      PPlayer = pawn::partyPlayer(PChar);
    const bool rowOn   = m_Gambits->MasterOn() && EatsWithPlayer();
    if (PPlayer == nullptr || !rowOn || !fed(PPlayer) || fed(PChar))
    {
        return false;
    }
    const auto                       pick = pawn::food::pickFor(PChar, pawn::roster::roleOf(PChar));
    const cardian::food::BeforeKneel kneel{
        .rowOn     = rowOn,
        .hasCookie = pick.has_value() && pick->cookie,
        .playerFed = true,
        .selfFed   = false,
        .kneeling  = aboutToKneel,
        .shortOfMp = shortOfMp,
    };
    if (!cardian::food::eatsBeforeKneel(kneel))
    {
        return false;
    }
    if (const auto status = pawn::food::eat(PChar, pick->itemId); status != CL_S_OK)
    {
        SayFood(fmt::format("could not eat her {} before she kneels (outcome 0x{:04X})", itemName(pick->itemId), status));
        return false;
    }
    m_FoodSaid.clear();
    ShowInfoFmt("rest: {} eats {} before she kneels (the player has food on)", POwner->getName(), itemName(pick->itemId));
    return true;
}
