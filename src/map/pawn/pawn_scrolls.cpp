// Cardian: a cardian of the player's own learns her spell scrolls (the user,
// 2026-10-25). A scroll in her inventory she can learn now -- one that
// arrives there, by his trade, his gift, her delivery box or a purchase, and
// one her new level opens -- is used as the game uses it, its own check and
// its learning, at her first free moment between fights, and she says so in
// party chat. Her other bags are where a scroll waits for someone else. One
// of the world's never: her spells are the census's.
#include "pawn_controller.h"
#include "pawn.h"
#include "pawn_items.h"
#include "role_support.h"
#include "scrolls.h"
#include "seats.h"

#include "ai/ai_container.h"
#include "ai/helpers/pathfind.h"
#include "cardian_link_protocol.h"
#include "common/logging.h"
#include "entities/char_entity.h"
#include "item_container.h"
#include "items/item.h"
#include "lua/luautils.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/charutils.h"

#include <chrono>

#include <fmt/format.h>

namespace
{
    constexpr auto kScrollLook  = std::chrono::seconds(4);  // how often her inventory is looked at
    constexpr auto kLearnLapses = std::chrono::seconds(15); // a use begun that taught nothing by then is let go
    constexpr auto kSkipFor     = std::chrono::minutes(5);  // a scroll whose use failed is left this long before it is tried again

    auto spellName(const uint16 spellId) -> std::string
    {
        CSpell* PSpell = spell::GetSpell(static_cast<SpellID>(spellId));
        return PSpell != nullptr ? PSpell->getName() : fmt::format("spell {}", spellId);
    }

    // The first scroll in her inventory she can learn now, by the game's own
    // check of a scroll (CLuaBaseEntity::canLearnSpell: not known, and her
    // jobs reach it), but for one left a while (`skip`): its slot and spell
    auto learnable(CCharEntity* PChar, const std::unordered_map<uint16, timer::time_point>& skip, const timer::time_point now)
        -> std::optional<std::pair<uint8, uint16>>
    {
        auto* PInventory = PChar->getStorage(LOC_INVENTORY);
        if (PInventory == nullptr)
        {
            return std::nullopt;
        }
        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            const CItem* PItem = PInventory->GetItem(slot);
            if (PItem == nullptr || PItem->isBusy())
            {
                continue;
            }
            const auto taught = pawn::scrolls::spellOf(PItem);
            if (!taught.has_value())
            {
                continue;
            }
            const auto spellId = static_cast<uint16>(*taught);
            if (const auto left = skip.find(spellId); left != skip.end() && now < left->second)
            {
                continue;
            }
            CSpell*    PSpell  = spell::GetSpell(*taught);
            if (PSpell == nullptr || !luautils::IsContentEnabled(PSpell->getContentTag()) || charutils::hasSpell(PChar, spellId) != 0 ||
                !spell::CanUseSpell(PChar, *taught))
            {
                continue;
            }
            return std::pair{ slot, spellId };
        }
        return std::nullopt;
    }
} // namespace

void CPawnController::ScrollTick()
{
    auto*      PChar = static_cast<CCharEntity*>(POwner);
    const auto now   = timer::now();

    // The use begun: the spell hers once its learning has run, said then
    if (m_Learning.has_value())
    {
        if (charutils::hasSpell(PChar, m_Learning->spellId) != 0)
        {
            const auto name = spellName(m_Learning->spellId);
            ShowInfoFmt("pawn: {} learns {} from its scroll", PChar->getName(), name);
            pawn::tactics::role::sayParty(PChar, fmt::format("Learned {}!", name));
            m_Learning.reset();
        }
        else if (now - m_Learning->since >= kLearnLapses)
        {
            ShowInfoFmt("pawn: {}'s scroll of {} taught her nothing; left a while", PChar->getName(), spellName(m_Learning->spellId));
            m_ScrollSkip[m_Learning->spellId] = now + kSkipFor;
            m_Learning.reset();
        }
        return;
    }
    if (now < m_ScrollLookAt)
    {
        return;
    }
    m_ScrollLookAt = now + kScrollLook;
    if (POwner->isDead() || pawn::seats::isWorlds(PChar->id) || PChar->tradePartner() != nullptr)
    {
        return;
    }

    // Her first moment between fights that she is free, as her food's
    // (FoodTick): standing still, done acting, nothing of his to carry out
    const auto* PPlayer       = pawn::partyPlayer(PChar);
    const bool  betweenFights = !POwner->PAI->IsEngaged() && (PPlayer == nullptr || !PPlayer->PAI->IsEngaged()) && PartyFightTarget() == nullptr &&
                               m_Mode != Mode::Fight && m_Mode != Mode::Hold && m_Mode != Mode::Attend;
    const bool  still         = POwner->PAI->PathFind == nullptr || !POwner->PAI->PathFind->IsFollowingPath();
    const bool  free          = still && !Acting() && ReadyToAct() && RestAllowsAction() && !InManeuver() && !HasQueuedOrder() && !HasPlayersOrder() &&
                      !m_Enchant.has_value() && !m_RestOrder.active() && m_Mode != Mode::Travel &&
                      !POwner->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
    if (!betweenFights || !free)
    {
        return;
    }

    const auto scroll = learnable(PChar, m_ScrollSkip, now);
    if (!scroll.has_value())
    {
        return;
    }
    if (const auto status = pawn::items::useItem(PChar, scroll->first, LOC_INVENTORY, false); status != CL_S_OK)
    {
        ShowInfoFmt("pawn: {} could not use her scroll of {} (outcome 0x{:04X}); left a while", PChar->getName(), spellName(scroll->second), status);
        m_ScrollSkip[scroll->second] = now + kSkipFor;
        return;
    }
    m_Learning = Learning{ .spellId = scroll->second, .since = now };
}
