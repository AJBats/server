// Cardian: observation only. No standing, walking, casting, or request feeding.
#include "cure_readiness.h"

#include "pawn_controller.h"
#include "fight_log.h"
#include "tactics.h"
#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "ai/states/ability_state.h"
#include "ai/states/item_state.h"
#include "ai/states/weaponskill_state.h"
#include "entities/char_entity.h"
#include "items/item_usable.h"
#include "recast_container.h"
#include "status_effect_container.h"
#include "utils/battleutils.h"

namespace pawn::tactics
{
    namespace
    {
        auto duration(const timer::duration value) -> double
        {
            return std::chrono::duration<double>(value).count();
        }

        // The active magic state's resolved timing includes its actual cast
        // modifiers and Quick Magic roll. Never call its mutating calculator.
        auto busyUntil(CCharEntity* body, CPawnController* control, const double now) -> double
        {
            double until = seconds(control->getLastSpellFinishedTime()) + 2.5;
            const auto* state = body->PAI->GetCurrentState();
            if (const auto* magic = dynamic_cast<const CMagicState*>(state))
            {
                until = std::max(until, seconds(magic->GetStartTime()) + duration(magic->GetCastTime()) +
                    std::max(2.5, duration(magic->GetSpell()->getAnimationTime())));
            }
            else if (const auto* ability = dynamic_cast<const CAbilityState*>(state))
            {
                until = std::max(until, seconds(ability->GetStartTime()) +
                    duration(ability->GetAbility()->getCastTime() + ability->GetAbility()->getAnimationTime()));
            }
            else if (const auto* item = dynamic_cast<const CItemState*>(state))
            {
                if (item->GetItem() == nullptr) return cardian::cure::unavailable; // consumed item, animation still finishing
                until = std::max(until, seconds(item->GetStartTime()) +
                    duration(item->GetItem()->getActivationTime() + item->GetItem()->getAnimationTime()));
            }
            else if (const auto* ws = dynamic_cast<const CWeaponSkillState*>(state))
            {
                until = std::max(until, seconds(ws->GetStartTime()) + duration(ws->GetSkill()->getAnimationTime()));
            }
            else if (control->Acting())
            {
                return cardian::cure::unavailable; // ranged attack has no exposed finish estimate
            }
            return std::max(0.0, until - now);
        }
    }

    auto measureCures(const Conveyor::Scope& scope, const double now) -> std::vector<cardian::cure::Option>
    {
        std::vector<cardian::cure::Option> out;
        for (auto* member : scope.members)
        {
            auto* body = dynamic_cast<CCharEntity*>(member);
            if (body == nullptr || body->isDead() || body->PAI == nullptr) continue;
            auto* control = dynamic_cast<CPawnController*>(body->PAI->GetController());
            const auto* casting = dynamic_cast<const CMagicState*>(body->PAI->GetCurrentState());
            // Observe actual player and pawn Cures already in flight too.
            if (casting != nullptr && !casting->IsCompleted() && casting->GetSpell()->getSpellFamily() == SPELLFAMILY_CURE)
            {
                auto* target = casting->target().resolve<CBattleEntity>();
                auto* spell = casting->GetSpell();
                if (target != nullptr && !target->isDead())
                {
                    const auto floor = cureEstimate(body->id, spell).first;
                    const double heals = bank::expectedCure(body, spell, target).value_or(floor);
                    out.push_back({body->id, target->id, static_cast<uint16>(spell->getID()), heals, 0.0,
                        {0.0, std::max(0.0, seconds(casting->GetStartTime()) + duration(casting->GetCastTime()) - now)}, 0.0, true});
                }
            }
            if (control == nullptr || !scope.holders.contains(body->id) || !control->Gambits().MasterOn()) continue;

            const double busy = busyUntil(body, control, now);
            const double stand = control->RestReadyIn(now);
            const double wakeCost = control->RestInterruptionCost();
            const bool blocked = control->HasQueuedOrder() || body->StatusEffectContainer->HasPreventActionEffect() ||
                body->StatusEffectContainer->HasStatusEffect({xi::StatusEffect::Silence, xi::StatusEffect::Mute});
            // MP is charged when an active cast lands. Reserve that cost
            // before advertising a follow-up Cure we cannot actually afford.
            const double committed = casting != nullptr && !casting->IsCompleted() &&
                !body->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Manafont)
                ? battleutils::CalculateSpellCost(body, casting->GetSpell()) : 0.0;
            const auto tiers = bank::cureTiers(body, bank::CureAvailability::Eligible);
            for (auto* target : scope.members)
            {
                if (target == nullptr || target->isDead() || target->loc.zone != body->loc.zone) continue;
                for (const auto& tier : tiers)
                {
                    const auto id = tier.id;
                    // First aid is her tactician's choice like any other
                    // cure: only a tier her allow-list lets her cast on this
                    // member (tactician_line.h)
                    if (!admittedBy(body, id, target).has_value()) continue;
                    cardian::cure::Option option{.caster = body->id, .target = target->id, .spell = static_cast<uint16>(id)};
                    auto* spell = spell::GetSpell(id);
                    option.heals = bank::expectedCure(body, spell, target).value_or(tier.option.heals);
                    option.mp = tier.option.mp;
                    option.wakeCost = wakeCost;
                    double recast = 0.0;
                    if (const auto* r = body->PRecastContainer->GetRecast(RECAST_MAGIC, static_cast<Recast>(id)))
                    {
                        recast = std::max(0.0, seconds(r->TimeStamp + r->RecastTime) - now);
                    }
                    if (casting != nullptr && !casting->IsCompleted() && casting->GetSpell()->getID() == id)
                    {
                        recast = std::max(recast, seconds(casting->GetStartTime()) + duration(casting->GetCastTime() + casting->GetRecast()) - now);
                    }
                    const double gap = std::max(0.0, static_cast<double>(distance(body->loc.p, target->loc.p) - bank::castRange(body, spell, target)));
                    // Path movement is a speed/40 step per 400-ms map tick.
                    // This is straight-line travel, not a navmesh/LOS promise.
                    const double walk = gap == 0.0 ? 0.0 : body->GetSpeed() > 0 &&
                        !body->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Bind) ?
                        (gap + 0.5) / (body->GetSpeed() / 16.0) : cardian::cure::unavailable;
                    // Future casts use base duration: no Quick Magic roll or
                    // consumption of a one-shot status during observation.
                    option.time = cardian::cure::timing(!blocked && option.mp <= body->health.mp - committed,
                        recast, stand, busy, walk, duration(spell->getCastTime()));
                    out.push_back(option);
                }
            }
        }
        return out;
    }
}
