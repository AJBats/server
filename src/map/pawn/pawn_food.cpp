// Cardian: her food (RESEARCH §19). Her "Self -> Eat with the player" row
// eats her party role's food when the player in her zone has food on and
// she has none, 2 to 7 seconds later by a roll of her own, at her first free
// moment between fights; a Healer or a mage whose food is a cookie eats it
// as she kneels for MP instead (RestTick); and a body of the world in the
// player's party is kept topped up, so she never runs out. What she eats is
// pawn/food.h's, the rules food_math.h's.
#include "food.h"
#include "party_roster.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "common/logging.h"
#include "common/xirand.h"
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

    auto sameZone(const CBaseEntity* PPlayer, const CBaseEntity* PPawn) -> bool
    {
        return PPlayer->loc.zone != nullptr && PPlayer->loc.zone == PPawn->loc.zone;
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
        m_FoodAt.reset();
        m_FoodDue.reset();
        return;
    }

    const auto now = timer::now();
    // A body of the world in his party never runs out (the user,
    // 2026-10-05); a recruit's food, and his own cardians', is his
    if (now >= m_FoodTopUpAt)
    {
        m_FoodTopUpAt = now + cardian::food::kTopUpEvery;
        if (pawn::seats::isWorlds(PChar->id))
        {
            pawn::food::topUp(PChar);
        }
    }

    cardian::food::WithPlayer moment{
        .rowOn     = m_Gambits->MasterOn() && EatsWithPlayer(),
        .playerFed = fed(PPlayer),
        .selfFed   = fed(PChar),
        .sameZone  = sameZone(PPlayer, POwner),
    };
    // no reason to eat, or it has gone: his food wore off, hers came on, the
    // row went off, he left her zone
    if (!cardian::food::hasReason(moment))
    {
        m_FoodAt.reset();
        m_FoodDue.reset();
        return;
    }
    // a new reason: her own delay after the player starts
    if (!m_FoodAt.has_value())
    {
        m_FoodAt = now + cardian::food::delayAfterPlayer(xirand::GetRandomNumber(cardian::food::kDelayMinMs, cardian::food::kDelayMaxMs + 1));
        return;
    }
    if (now < *m_FoodAt)
    {
        return;
    }
    if (!m_FoodDue.has_value())
    {
        const auto role = pawn::roster::roleOf(PChar);
        const auto pick = pawn::food::pickFor(PChar, role);
        if (!pick.has_value())
        {
            SayFood(fmt::format("has no food for her role ({}) to eat with the player", cardian::party::roleName(role)));
            m_FoodAt = now + cardian::food::kLookAgain;
            return;
        }
        if (pick->cookie)
        {
            SayFood(fmt::format("keeps her {} for her next kneel", itemName(pick->itemId)));
            m_FoodAt = now + cardian::food::kLookAgain;
            return;
        }
        m_FoodDue = pick->itemId;
    }

    // Due, she eats at her first moment between fights that she is free:
    // standing still (a step cuts an item's use short), up and done standing
    // from a kneel (the rest policy's word: a use asked sooner is refused),
    // not acting, nothing of his to carry out, no enchanted item on its way,
    // no rest order on
    moment.betweenFights = !POwner->PAI->IsEngaged() && !PPlayer->PAI->IsEngaged() && PartyFightTarget() == nullptr && m_Mode != Mode::Fight &&
                           m_Mode != Mode::Hold && m_Mode != Mode::Attend;
    const bool still = POwner->PAI->PathFind == nullptr || !POwner->PAI->PathFind->IsFollowingPath();
    moment.free      = still && !Acting() && ReadyToAct() && RestAllowsAction() && !InManeuver() && !HasQueuedOrder() && !HasPlayersOrder() &&
                  !m_Enchant.has_value() && !m_RestOrder.active() && m_Mode != Mode::Travel &&
                  !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (!cardian::food::eatsWithPlayer(moment))
    {
        return;
    }
    const auto itemId = *m_FoodDue;
    m_FoodDue.reset();
    // her food's effect comes on as the use ends; one that never does is
    // looked at again then
    m_FoodAt = now + cardian::food::kLookAgain;
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
    if (PPlayer == nullptr || !rowOn || !fed(PPlayer) || fed(PChar) || !sameZone(PPlayer, POwner))
    {
        return false;
    }
    const auto                       pick = pawn::food::pickFor(PChar, pawn::roster::roleOf(PChar));
    const cardian::food::BeforeKneel kneel{
        .rowOn     = rowOn,
        .hasCookie = pick.has_value() && pick->cookie,
        .playerFed = true,
        .selfFed   = false,
        .sameZone  = true,
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
