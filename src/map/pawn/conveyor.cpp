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

        // Her tactician's own proposal still has an allow-list row behind it
        // (tactician_line.h): a named spell one for that spell on that
        // target, a Cure left to the bank one for some tier on that member
        auto stillAdmitted(CCharEntity* PCaster, const Need& n, const Request& r, CBattleEntity* PTarget) -> bool
        {
            if (PTarget == nullptr)
            {
                return false;
            }
            if (r.spell != 0)
            {
                return admittedBy(PCaster, static_cast<SpellID>(r.spell), PTarget).has_value();
            }
            if (n.key.kind != NeedKind::Cure)
            {
                return true;
            }
            return std::ranges::any_of(cardian::tactician::kCureTiers, [&](const uint16 tier)
                                       {
                                           return admittedBy(PCaster, static_cast<SpellID>(tier), PTarget).has_value();
                                       });
        }

        // She fed this need from a row of her own above her line: her order,
        // with every tier and spell she has
        auto hersToOrder(const Need& n, const uint32 caster) -> bool
        {
            return std::ranges::any_of(n.requests, [caster](const Request& r)
                                       {
                                           return r.source == Source::Row && r.caster == caster;
                                       });
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

        // What a member's cure in flight is expected to heal: the cure
        // formula run on its caster, else what she is known to heal with it,
        // a floor counted double as the line counts one (bank_math.h wholeAt),
        // so a top-up errs towards not overcuring
        auto inFlightHeal(CBattleEntity* PMember) -> int32
        {
            auto*   PState  = static_cast<CMagicState*>(PMember->PAI->GetCurrentState());
            CSpell* PSpell  = PState->GetSpell();
            auto*   PTarget = PState->target().resolve<CBattleEntity>();
            if (PSpell == nullptr || PTarget == nullptr)
            {
                return 0;
            }
            if (const auto heals = bank::expectedCure(PMember, PSpell, PTarget); heals.has_value())
            {
                return *heals;
            }
            const auto [heals, exact] = cureEstimate(PMember->id, PSpell);
            return exact ? heals : 2 * heals;
        }

        // A request still stands: a row the player has not changed or
        // deleted since it fed, a proposal whose allow-list row is still
        // there, and the reflex always
        auto requestStands(const Need& n, const Request& r, CBattleEntity* PTarget) -> bool
        {
            if (r.source == Source::Reflex)
            {
                return true;
            }
            auto* PCaster    = zoneutils::GetChar(r.caster);
            auto* controller = controllerOf(PCaster);
            if (controller == nullptr)
            {
                return false;
            }
            if (r.source == Source::Row)
            {
                return controller->Gambits().RequestValid(r.rowId, n.key.target, r.spell);
            }
            return stillAdmitted(PCaster, n, r, PTarget);
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
        m_incoming.clear();
        m_inFlight.clear();
        for (auto* PMember : scope.members)
        {
            if (const auto key = castingKey(PMember); key.has_value())
            {
                auto& n    = m_needs.open(*key);
                n.lockedBy = PMember->id;
                n.assigned = 0; // a locked need is assigned again only by the top-up pass
                casting.insert(PMember->id);
                m_pending[PMember->id] = *key;
                if (key->kind == NeedKind::Cure)
                {
                    const int32 heals = inFlightHeal(PMember);
                    m_incoming[key->target] += heals;
                    m_inFlight[PMember->id] = { key->target, heals };
                }
            }
        }
        // And the emergency cures chosen for this tick, not yet begun: a
        // top-up must not land on top of one
        for (const auto& choice : m_emergency)
        {
            if (choice.cast && !casting.contains(choice.cure.caster))
            {
                m_incoming[choice.cure.target] += static_cast<int32>(choice.cure.heals);
            }
        }
        std::erase_if(m_pending, [&](const auto& kv)
                      {
                          return !casting.contains(kv.first);
                      });

        // A queued row is still the player's condition, not a four-second
        // promise to cast after the condition or the editor has changed.
        // So is her tactician's own proposal: the allow-list row that let it
        // is asked again, so a row deleted mid-fight stops her at once
        for (auto& n : m_needs.needs)
        {
            if (n.lockedBy != 0)
            {
                continue;
            }
            auto* PTarget = resolve(scope, n.key.target);
            std::erase_if(n.requests, [&](const Request& r)
            {
                return !requestStands(n, r, PTarget);
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
        // Then the top-ups: a cure already in flight leaves its need open to
        // another mage whose own cure still lands whole on what it leaves.
        // A locked need keeps every request for the retry stamps it owes
        // (Needs::expire), so the top-up is scheduled on the requests that
        // still stand: a row deleted mid-cast names nobody
        for (auto& n : m_needs.needs)
        {
            if (n.lockedBy == 0 || n.key.kind != NeedKind::Cure)
            {
                continue;
            }
            auto* PTarget = resolve(scope, n.key.target);
            Need  standing = n;
            std::erase_if(standing.requests, [&](const Request& r)
                          {
                              return !requestStands(n, r, PTarget);
                          });
            schedule(standing, scope, loads);
            n.assigned = standing.assigned;
            n.spell    = standing.spell;
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
        // Her cure is no longer in flight: its heal leaves the gap it was
        // counted against, before anything is fed again this tick
        if (const auto it = m_inFlight.find(PCaster->id); it != m_inFlight.end())
        {
            auto& incoming = m_incoming[it->second.first];
            incoming       = std::max(0, incoming - it->second.second);
            m_inFlight.erase(it);
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
            // The rows that asked start their retry clocks now, whoever cast
            // it: the orders that fed it, and the allow-list rows that let
            // her tactician's own proposals
            for (const auto& r : n->requests)
            {
                if (r.source == Source::Reflex || r.rowId.empty())
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
        // Emergency aid may have several Cures in flight. One finishing
        // must not unlock ordinary duplicate requests while another remains.
        for (const auto& [caster, pending] : m_pending)
        {
            if (pending == *key)
            {
                n->lockedBy = caster;
                n->assigned = 0;
                return;
            }
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
            if (std::any_of(m_emergency.begin(), m_emergency.end(), [&](const auto& c) { return c.cast && c.cure.caster == PChar->id; }))
            {
                continue; // this slot is reserved for first aid, including preparation
            }
            cardian::tactics::Candidate c{ .id = PChar->id };
            c.spell = static_cast<uint16>(spellFor(n, PChar, PTarget));
            // In range as the magic state will judge the cast: the spell's
            // own range plus both hitboxes
            auto* PSpell = c.spell != 0 ? spell::GetSpell(static_cast<SpellID>(c.spell)) : nullptr;
            const float reach  = bank::castRange(PChar, PSpell, PTarget);
            c.open             = PSpell != nullptr && !PChar->isDead() && PChar->loc.zone == PTarget->loc.zone &&
                     PController->Gambits().MasterOn() && PController->RestAllowsAction() && !PController->Acting() && !PController->HasQueuedOrder() && PController->canAct();
            c.open = c.open && !PChar->StatusEffectContainer->HasPreventActionEffect() &&
                     !PChar->StatusEffectContainer->HasStatusEffect({xi::StatusEffect::Silence, xi::StatusEffect::Mute});
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
            it = m_tiers.emplace(PCaster->id, bank::cureTiers(PCaster, bank::CureAvailability::Ready)).first;
        }
        return it->second;
    }

    auto Conveyor::spellFor(const Need& n, CCharEntity* PCaster, CBattleEntity* PTarget) -> SpellID
    {
        // Her own row fed it: an order, cast with anything she has. Anything
        // else is her tactician's choice -- her role's own proposal, or
        // another mage's row she takes over -- and only what her allow-list
        // lets her cast on this target (tactician_line.h)
        const bool order   = hersToOrder(n, PCaster->id);
        const auto allowed = [&](const SpellID id)
        {
            return order || admittedBy(PCaster, id, PTarget).has_value();
        };
        // The per-caster cache stays whole; the tiers she may cast on this
        // member are picked from it here
        const auto allowedTiers = [&]
        {
            std::vector<bank::CureTier> tiers;
            for (const auto& tier : tiersOf(PCaster))
            {
                if (allowed(tier.id))
                {
                    tiers.push_back(tier);
                }
            }
            return tiers;
        };
        if (n.key.kind == NeedKind::Cure)
        {
            // Explicit rows may cure a whole member, asleep or awake. The
            // role alone avoids overcure. A delegated row keeps its spell.
            if (PTarget->isDead() || (!n.rowFed() && PTarget->health.hp >= PTarget->GetMaxHP()))
            {
                return static_cast<SpellID>(0);
            }
            const uint16 asked = n.askedSpell();
            // The role's own cure, and any mage's top-up of a cure in flight:
            // the biggest tier she may cast that lands whole on the gap the
            // cures in flight leave (bank_math.h pickWhole). A top-up of a
            // row that names its spell casts that spell only if it lands whole
            if (n.lockedBy != 0 || !n.rowFed())
            {
                const auto  in    = m_incoming.find(PTarget->id);
                const int32 gap   = PTarget->GetMaxHP() - PTarget->health.hp - (in != m_incoming.end() ? in->second : 0);
                auto        tiers = allowedTiers();
                if (asked != 0)
                {
                    std::erase_if(tiers, [asked](const bank::CureTier& tier)
                                  {
                                      return static_cast<uint16>(tier.id) != asked;
                                  });
                }
                return bank::pickTierWhole(tiers, gap);
            }
            if (asked != 0)
            {
                const auto id = static_cast<SpellID>(asked);
                return bank::usable(PCaster, id) && allowed(id) ? id : static_cast<SpellID>(0);
            }
            return bank::pickTier(allowedTiers(), PTarget, true); // a row's: every tier when it is her order
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
        const auto spell = static_cast<SpellID>(id);
        return id != 0 && bank::usable(PCaster, spell) && allowed(spell) ? spell : static_cast<SpellID>(0);
    }

    auto Conveyor::describe(const Need& n, const Scope& scope) const -> std::string
    {
        // a top-up says whose cure is already in flight
        const auto after = n.lockedBy != 0 ? fmt::format(", after {}'s cure", nameOf(scope, n.lockedBy)) : std::string();
        const auto& lead = n.lead();
        switch (lead.source)
        {
            case Source::Reflex:
                return fmt::format("the reflex: {}{}", lead.why, after);
            case Source::Row:
                return (lead.caster == n.assigned ? fmt::format("her row {}", lead.row) : fmt::format("{}'s row {}", nameOf(scope, lead.caster), lead.row)) + after;
            case Source::Role:
                return fmt::format("the role: {}{}", lead.why, after);
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
        for (const auto& choice : m_emergency)
        {
            if (choice.cure.caster != caster || !choice.cast) continue;
            auto* target = resolve(scope, choice.cure.target);
            if (target == nullptr || target->isDead()) return std::nullopt;
            // Emergency selection already accounted for the amount/timing
            // of Cures in flight. Its additional Cure may bypass that lock.
            return Assignment{static_cast<SpellID>(choice.cure.spell), choice.cure.target,
                "emergency aid: earliest useful Cure", true, true};
        }
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
        for (const auto& choice : m_emergency)
        {
            const auto& c = choice.cure;
            out.push_back(fmt::format("Emergency {} -> {}: {} #{}, starts in {:.1f}s, lands in {:.1f}s, ~{:.0f} HP",
                nameOf(scope, c.caster), nameOf(scope, c.target), choice.cast ? "Cure" : "prepare Cure",
                c.spell, c.time.start, c.time.land, c.heals));
        }
        for (const auto& n : m_needs.needs)
        {
            std::string voices;
            for (const auto& r : n.requests)
            {
                voices += (voices.empty() ? "" : ", ") + voice(scope, r);
            }
            std::string state;
            if (n.lockedBy != 0 && n.assigned != 0)
            {
                auto* PSpell = spell::GetSpell(static_cast<SpellID>(n.spell));
                state        = fmt::format("{} is casting it, {} tops up with {}", nameOf(scope, n.lockedBy), nameOf(scope, n.assigned), PSpell != nullptr ? PSpell->getName() : "?");
            }
            else if (n.lockedBy != 0)
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
