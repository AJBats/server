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

#include "conveyor.h"

#include "fight_log.h"
#include "pawn_controller.h"
#include "pawn_gambits.h"
#include "tactics.h"

#include "common/logging.h"

#include "ai/ai_container.h"
#include "ai/states/magic_state.h"
#include "entities/char_entity.h"
#include "spell.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"

#include <algorithm>
#include <optional>

namespace pawn::tactics
{
    namespace
    {
        auto controllerOf(CCharEntity* PChar) -> CPawnController*
        {
            return PChar != nullptr && PChar->PAI != nullptr ? dynamic_cast<CPawnController*>(PChar->PAI->GetController()) : nullptr;
        }

        auto nameOf(const Conveyor::Scope& scope, const uint32 id) -> std::string
        {
            const auto* PEntity = Conveyor::resolve(scope, id);
            return PEntity != nullptr ? PEntity->getName() : fmt::format("#{}", id);
        }

        auto kindName(const NeedKey& key) -> std::string
        {
            switch (key.kind)
            {
                case NeedKind::Cure:
                    return "Cure";
                case NeedKind::Status:
                    return pawn::familyName(key.arg);
                case NeedKind::Damage:
                    return "damage";
                default:
                {
                    auto* PSpell = spell::GetSpell(static_cast<SpellID>(key.arg));
                    return PSpell != nullptr ? PSpell->getName() : fmt::format("spell {}", key.arg);
                }
            }
        }

        auto voice(const Conveyor::Scope& scope, const Request& r) -> std::string
        {
            switch (r.source)
            {
                case Source::Reflex:
                    return fmt::format("the reflex ({})", r.why);
                case Source::Row:
                    return fmt::format("{}'s row {}", nameOf(scope, r.caster), r.row);
                case Source::Role:
                    return fmt::format("the role ({})", r.why);
            }
            return "?";
        }

        auto castingKey(CBattleEntity* PMember) -> std::optional<NeedKey>
        {
            if (PMember == nullptr || PMember->PAI == nullptr || !PMember->PAI->IsCurrentState<CMagicState>())
            {
                return std::nullopt;
            }
            auto*       PState  = static_cast<CMagicState*>(PMember->PAI->GetCurrentState());
            CSpell*     PSpell  = PState->GetSpell();
            const auto* PTarget = PState->target().resolve();
            if (PSpell == nullptr)
            {
                return std::nullopt;
            }
            return Conveyor::keyFor(PSpell, PTarget != nullptr ? PTarget->id : 0, PMember->id);
        }
    } // namespace

    auto Conveyor::keyFor(CSpell* PSpell, uint32 target, const uint32 caster) -> NeedKey
    {
        if (PSpell == nullptr)
        {
            return { NeedKind::Cure, 0, target }; // a "best cure" row: the bank picks the tier
        }
        if (PSpell->getValidTarget() == TARGET_SELF)
        {
            target = caster;
        }
        if (PSpell->isCure() && PSpell->getSpellFamily() == SPELLFAMILY_CURE)
        {
            return { NeedKind::Cure, 0, target };
        }
        const auto family = PSpell->getSpellFamily();
        if (family != SPELLFAMILY_NONE && (PSpell->isDebuff() || PSpell->isBuff() || PSpell->isNa() || PSpell->isHeal()))
        {
            return { NeedKind::Status, static_cast<uint32>(family), target };
        }
        if (PSpell->dealsDamage())
        {
            return { NeedKind::Damage, caster, target };
        }
        return { NeedKind::Other, static_cast<uint32>(PSpell->getID()), target };
    }

    auto Conveyor::resolve(const Scope& scope, const uint32 id) -> CBattleEntity*
    {
        for (auto* PMember : scope.members)
        {
            if (PMember != nullptr && PMember->id == id)
            {
                return PMember;
            }
        }
        auto* PEntity = zoneutils::GetEntity(id, TYPE_MOB);
        return PEntity != nullptr && PEntity->id == id ? dynamic_cast<CBattleEntity*>(PEntity) : nullptr;
    }

    auto Conveyor::feed(const NeedKey& key, Request r, const Scope& scope) -> const Need&
    {
        if (debug())
        {
            // A new voice on a need, once; a re-feed is the same voice again
            const auto* have  = m_needs.find(key);
            const bool  known = have != nullptr && std::any_of(have->requests.begin(), have->requests.end(), [&](const Request& q)
                                                                {
                                                                    return q.same(r);
                                                                });
            if (!known)
            {
                ShowInfoFmt("tactics: {} fed for {} on {}", voice(scope, r), kindName(key), nameOf(scope, key.target));
            }
        }
        auto& n = m_needs.feed(key, std::move(r));
        if (n.lockedBy == 0)
        {
            std::unordered_map<uint32, uint32> loads;
            for (const auto& other : m_needs.needs)
            {
                if (other.assigned != 0 && &other != &n)
                {
                    ++loads[other.assigned];
                }
            }
            schedule(n, scope, loads);
        }
        return n;
    }

    void Conveyor::tick(const double now, const double life, const Scope& scope)
    {
        m_tiers.clear();

        // The casts in flight, the player's included: each is its own
        // locked need, opened if nobody fed it
        for (auto& n : m_needs.needs)
        {
            n.lockedBy = 0;
        }
        std::unordered_set<uint32> casting;
        for (auto* PMember : scope.members)
        {
            if (const auto key = castingKey(PMember); key.has_value())
            {
                auto& n    = m_needs.open(*key);
                n.lockedBy = PMember->id;
                n.assigned = 0;
                casting.insert(PMember->id);
                m_pending[PMember->id] = *key;
            }
        }
        std::erase_if(m_pending, [&](const auto& kv)
                      {
                          return !casting.contains(kv.first);
                      });

        // A queued row is still the player's condition, not a four-second
        // promise to cast after the condition or the editor has changed.
        for (auto& n : m_needs.needs)
        {
            if (n.lockedBy != 0)
            {
                continue;
            }
            std::erase_if(n.requests, [&](const Request& r)
            {
                if (r.source != Source::Row)
                {
                    return false;
                }
                auto* controller = controllerOf(zoneutils::GetChar(r.caster));
                return controller == nullptr || !controller->Gambits().RequestValid(r.rowId, n.key.target, r.spell);
            });
        }
        m_needs.expire(now, life);

        std::unordered_map<uint32, uint32> loads;
        for (auto& n : m_needs.needs)
        {
            if (n.lockedBy == 0)
            {
                n.assigned = 0;
            }
        }
        for (auto& n : m_needs.needs)
        {
            if (n.lockedBy == 0)
            {
                schedule(n, scope, loads);
            }
        }
    }

    void Conveyor::castStarted(CCharEntity* PCaster, CSpell* PSpell, const uint32 target)
    {
        if (PCaster == nullptr || PSpell == nullptr)
        {
            return;
        }
        const auto key = keyFor(PSpell, target, PCaster->id);
        auto&      n   = m_needs.open(key);
        n.lockedBy     = PCaster->id;
        n.assigned     = 0;
        m_pending[PCaster->id] = key;
    }

    void Conveyor::castEnded(CCharEntity* PCaster, CSpell* PSpell, const uint32 target, const bool landed)
    {
        if (PCaster == nullptr)
        {
            return;
        }
        std::optional<NeedKey> key;
        if (const auto it = m_pending.find(PCaster->id); it != m_pending.end())
        {
            key = it->second;
            m_pending.erase(it);
        }
        else if (PSpell != nullptr)
        {
            key = keyFor(PSpell, target, PCaster->id);
        }
        if (!key.has_value())
        {
            return;
        }
        auto* n = m_needs.find(*key);
        if (n == nullptr)
        {
            return;
        }
        if (landed)
        {
            // The rows that asked start their retry clocks now, whoever cast it
            for (const auto& r : n->requests)
            {
                if (r.source != Source::Row)
                {
                    continue;
                }
                if (auto* PController = controllerOf(zoneutils::GetChar(r.caster)); PController != nullptr)
                {
                    PController->Gambits().StampRetry(r.rowId, timer::now());
                }
            }
        }
        if (debug())
        {
            ShowInfoFmt("tactics: {} on #{} {} by {}", kindName(*key), key->target, landed ? "met" : "not met", PCaster->getName());
        }
        m_needs.forget(*key);
    }

    void Conveyor::schedule(Need& n, const Scope& scope, std::unordered_map<uint32, uint32>& loads)
    {
        n.assigned = 0;
        n.spell    = 0;
        auto* PTarget = resolve(scope, n.key.target);
        if (PTarget == nullptr)
        {
            return;
        }
        // Who may cast it: whoever fed it, and for a need a row asked for,
        // every role holder; damage and the rest are the row's mage's alone
        const bool rowFed = n.rowFed();
        const bool hers   = n.key.kind == NeedKind::Damage || n.key.kind == NeedKind::Other;

        std::vector<cardian::tactics::Candidate> candidates;
        for (auto* PMember : scope.members)
        {
            if (PMember == nullptr || PMember->objtype != TYPE_PC)
            {
                continue;
            }
            auto*      PChar    = static_cast<CCharEntity*>(PMember);
            const bool fed      = n.fedBy(PChar->id);
            const bool eligible = hers ? PChar->id == n.preferred() : (fed || (rowFed && scope.holders.contains(PChar->id)));
            if (!eligible)
            {
                continue;
            }
            // A real player is watched, never commanded: no controller, no candidate
            auto* PController = controllerOf(PChar);
            if (PController == nullptr)
            {
                continue;
            }
            cardian::tactics::Candidate c{ .id = PChar->id };
            c.spell = static_cast<uint16>(spellFor(n, PChar, PTarget));
            // In range as the magic state will judge the cast: the spell's
            // own range plus both hitboxes
            auto* PSpell = c.spell != 0 ? spell::GetSpell(static_cast<SpellID>(c.spell)) : nullptr;
            const float reach  = bank::castRange(PChar, PSpell, PTarget);
            c.open             = PSpell != nullptr && !PChar->isDead() && PChar->loc.zone == PTarget->loc.zone &&
                     PController->Gambits().MasterOn() && !PController->Acting() && !PController->HasQueuedOrder() && PController->canAct();
            c.inRange          = distance(PChar->loc.p, PTarget->loc.p) <= reach;
            // If nobody can cast now, the row's own mage can approach.
            // Role-only needs retain their position/range policy.
            c.open = c.open && (c.inRange || (rowFed && PChar->id == n.preferred()));
            c.kneeling = PChar->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
            for (const auto& r : n.requests)
            {
                if (r.source == Source::Role && r.caster == PChar->id)
                {
                    c.landChance = r.landChance;
                }
            }
            if (const auto it = loads.find(PChar->id); it != loads.end())
            {
                c.load = it->second;
            }
            candidates.push_back(c);
        }

        if (const auto* pick = cardian::tactics::pickCaster(n, candidates); pick != nullptr)
        {
            n.assigned = pick->id;
            n.spell    = pick->spell;
            ++loads[pick->id];
        }
    }

    auto Conveyor::tiersOf(CCharEntity* PCaster) -> const std::vector<bank::CureTier>&
    {
        auto it = m_tiers.find(PCaster->id);
        if (it == m_tiers.end())
        {
            it = m_tiers.emplace(PCaster->id, bank::cureTiers(PCaster, true)).first;
        }
        return it->second;
    }

    auto Conveyor::spellFor(const Need& n, CCharEntity* PCaster, CBattleEntity* PTarget) -> SpellID
    {
        if (n.key.kind == NeedKind::Cure)
        {
            // Explicit rows may cure a whole member, asleep or awake. The
            // role alone avoids overcure. A delegated row keeps its spell.
            if (PTarget->isDead() || (!n.rowFed() && PTarget->health.hp >= PTarget->GetMaxHP()))
            {
                return static_cast<SpellID>(0);
            }
            const uint16 asked = n.askedSpell();
            if (asked != 0)
            {
                return bank::usable(PCaster, static_cast<SpellID>(asked)) ? static_cast<SpellID>(asked) : static_cast<SpellID>(0);
            }
            return bank::pickTier(tiersOf(PCaster), PTarget, n.rowFed());
        }
        // A row's named spell takes precedence, including when another
        // mage casts it. With no row, use this role holder's own proposal.
        uint16 id = n.askedSpell();
        for (const auto& r : n.requests)
        {
            if (id == 0 && r.caster == PCaster->id && r.spell != 0)
            {
                id = r.spell;
                break;
            }
        }
        if (id == 0)
        {
            id = n.askedSpell();
        }
        return id != 0 && bank::usable(PCaster, static_cast<SpellID>(id)) ? static_cast<SpellID>(id) : static_cast<SpellID>(0);
    }

    auto Conveyor::describe(const Need& n, const Scope& scope) const -> std::string
    {
        const auto& lead = n.lead();
        switch (lead.source)
        {
            case Source::Reflex:
                return fmt::format("the reflex: {}", lead.why);
            case Source::Row:
                return lead.caster == n.assigned ? fmt::format("her row {}", lead.row) : fmt::format("{}'s row {}", nameOf(scope, lead.caster), lead.row);
            case Source::Role:
                return fmt::format("the role: {}", lead.why);
        }
        return "";
    }

    auto Conveyor::offensive(const Need& n, const Scope& scope) -> bool
    {
        if (n.key.kind == NeedKind::Damage)
        {
            return true;
        }
        if (n.key.kind == NeedKind::Cure || n.key.kind == NeedKind::Na)
        {
            return false;
        }
        const auto* PTarget = resolve(scope, n.key.target);
        return PTarget != nullptr && PTarget->objtype == TYPE_MOB;
    }

    auto Conveyor::assignment(const uint32 caster, const bool engaged, const Scope& scope) const -> std::optional<Assignment>
    {
        for (const auto index : cardian::tactics::slotOrder(m_needs.needs, caster))
        {
            const auto& n = m_needs.needs[index];
            if (!engaged && offensive(n, scope))
            {
                continue;
            }
            return Assignment{ static_cast<SpellID>(n.spell), n.key.target, describe(n, scope), n.rowFed() };
        }
        return std::nullopt;
    }

    auto Conveyor::lines(const Scope& scope) const -> std::vector<std::string>
    {
        std::vector<std::string> out;
        for (const auto& n : m_needs.needs)
        {
            std::string voices;
            for (const auto& r : n.requests)
            {
                voices += (voices.empty() ? "" : ", ") + voice(scope, r);
            }
            std::string state;
            if (n.lockedBy != 0)
            {
                state = fmt::format("{} is casting it", nameOf(scope, n.lockedBy));
            }
            else if (!n.held())
            {
                auto* PSpell = spell::GetSpell(static_cast<SpellID>(n.spell));
                state        = fmt::format("{} casts {} ({})", nameOf(scope, n.assigned), PSpell != nullptr ? PSpell->getName() : "?", describe(n, scope));
            }
            else
            {
                state = "held, nobody open";
            }
            out.push_back(fmt::format("need: {} on {}, fed by {}: {}", kindName(n.key), nameOf(scope, n.key.target), voices.empty() ? "nobody" : voices, state));
        }
        return out;
    }
} // namespace pawn::tactics
