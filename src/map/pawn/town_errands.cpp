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

#include "town_errands.h"
#include "town_errand_math.h"

#include "errands.h"
#include "gate_guards.h"
#include "pawn.h"
#include "pawn_controller.h"
#include "pawn_travel.h"
#include "redress.h"
#include "seats.h"
#include "supplies.h"
#include "world.h"

#include "ai/ai_container.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/xirand.h"
#include "data/datasets/zones/settings/dataset.h"
#include "data/loader.h"
#include "entities/char_entity.h"
#include "enums/chat_message_type.h"
#include "packets/s2c/0x017_chat_std.h"
#include "party.h"
#include "status_effect_container.h"
#include "utils/zoneutils.h"
#include "zone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pawn::town
{
    namespace
    {
        using namespace std::chrono_literals;
        using cardian::town::Busy;
        using cardian::town::Line;
        using cardian::town::NeedFacts;
        using cardian::town::OnFacts;
        using cardian::town::Spot;
        using cardian::town::Stop;
        using cardian::town::Who;
        using cardian::town::WhoFacts;

        constexpr auto  kLookEvery  = 1s;   // a zone's cardians, and the errands under way
        constexpr auto  kWalkFor    = 120s; // a walk to a stop, or back to her seat: past it she goes on without it
        constexpr auto  kTrekFor    = 480s; // the way to her consulate in another zone of her city
        constexpr auto  kCensusWait = 60s;  // the census's answer at the counter: past it she goes on without
        constexpr auto  kHeardAfter = 2s;   // the player in a zone this long hears her: his client is up
        constexpr auto  kOwedFor    = 20s;  // a line he could not hear in this long is let go
        constexpr auto  kRecentLine = 10s;  // a line said this recently is passed over by the next to speak
        constexpr auto  kJoinedFor  = 10s;  // a look this soon after an invite answers it: her ready line if she needs nothing
        constexpr auto  kRetryAfter = 600s; // a need she failed at -- a stop given up, a purchase refused -- waits this long, wherever she goes
        constexpr uint32 kStaggerMin = 5;   // seconds: one of the world's sets out this long after she is free, at the least
        constexpr uint32 kStaggerMax = 60;  // ... and at the most, so a town never empties its seats at once
        constexpr float kAtStop     = 5.0f; // yalms from the counter, the guard, the door or the moogle: she is at it
        constexpr float kAtSeat     = 2.0f; // yalms from her seat: she is back
        constexpr float kAtGoal     = 1.5f; // yalms from the floor beside a stop: she is there

        // A stop of hers: where she sees to a need
        struct StopAt
        {
            Stop                       stop = Stop::Counter;
            position_t                 at{};            // the counter, the guard, a Mog House door's ground or the moogle
            const pawn::guards::Guard* guard = nullptr; // a guard's, a consulate's
            uint16                     zone  = 0;       // a consulate's zone
        };

        enum class Phase : uint8
        {
            Walk, // to the stop
            Trek, // to her consulate's zone
            At,   // at the counter, waiting on the census
            Back, // one of the world's, to her seat
        };

        // The errands under way: the map's alone, gone with a restart
        struct Errand
        {
            Who                       who     = Who::Nobody;
            uint32                    player  = 0; // in his party: whose
            uint16                    zone    = 0; // the zone she set out in
            uint16                    hisZone = 0; // his zone as she set out
            position_t                from{};      // where she stood: one of the world's walks back to it
            std::vector<StopAt>       stops;
            std::size_t               next  = 0;
            Phase                     phase = Phase::Walk;
            timer::time_point         since{};      // the phase's start, on the simulation clock (a pause holds it)
            std::optional<position_t> goal;         // her walk order now
            bool                      asked        = false; // the census asked at the counter
            bool                      afresh       = false; // her jobs changed: the counter asks for her whatever she was dressed for
            bool                      jobsCounter  = false; // the counter is a stop for the job change alone: no change, no counter
            bool                      saidNotReady = false;
        };

        // Each cardian as the last look saw her
        struct Watch
        {
            uint16 zone       = 0;
            bool   free       = false; // someone's, and nothing interrupting her
            bool   pending    = false; // a look owed, once nothing she is in the middle of keeps her
            uint8  seenTo     = 0;     // the needs whose stop she reached in this zone, or that nothing here sees to
            bool   setOutHere = false; // she has set out in this zone already: taken up again, without a word
            uint8  lacked     = 0;     // the scrolls missing from her bags, as the last look saw her (supplies::lacks)
            uint8  failed     = 0;     // the needs she failed at lately, wherever she was: not looked for until kRetryAfter
            Who    who        = Who::Nobody; // whose she was, as the last look saw her
            timer::time_point failedAt{};  // the last failure
            timer::time_point joinedAt{};  // her invite accepted, until a look answers it (noteJoined)
            timer::time_point notBefore{}; // the owed look waits until then
        };

        // A line she owes her player, said once he hears it
        struct Owed
        {
            uint32            charid = 0;
            Line              line   = Line::Ready;
            uint32            player = 0;
            timer::time_point at{};
        };

        // Where a player is and since when: a zone line leaves him nowhere for
        // a moment, and his client a moment more
        struct Heard
        {
            uint16            zone = 0;
            timer::time_point since{};
        };

        // The lines of a kind said a moment ago, a bit each
        struct Recent
        {
            uint32            mask = 0;
            timer::time_point at{};
        };

        std::unordered_map<uint32, Errand>            errands; // by her charid
        std::unordered_map<uint32, Watch>             watches; // by her charid
        std::vector<Owed>                             owed;
        std::unordered_map<uint32, Heard>             heard; // by his charid
        std::array<Recent, 3>                         recent{};
        std::unordered_map<uint16, timer::time_point> lookedAt; // by zone
        timer::time_point                             steppedAt{};

        auto isCity(CZone* PZone) -> bool
        {
            return PZone != nullptr && (PZone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
        }

        auto controllerOf(CCharEntity* PPawn) -> CPawnController*
        {
            return PPawn->PAI != nullptr ? dynamic_cast<CPawnController*>(PPawn->PAI->GetController()) : nullptr;
        }

        auto zoneNameOf(const CCharEntity* PPawn) -> std::string
        {
            return PPawn->loc.zone != nullptr ? std::string(PPawn->loc.zone->getName()) : std::string("nowhere");
        }

        auto needsText(const uint8 needs) -> std::string
        {
            std::string out;
            const auto  add = [&](const uint8 bit, const char* word)
            {
                if ((needs & bit) != 0)
                {
                    out += out.empty() ? "" : ", ";
                    out += word;
                }
            };
            add(cardian::town::kRedress, "re-dress");
            add(cardian::town::kScrolls, "scrolls");
            add(cardian::town::kJobs, "job change");
            return out;
        }

        // ---- who she is, and what keeps her ---------------------------------------

        auto whoOf(CCharEntity* PPawn) -> WhoFacts
        {
            const uint32 id = PPawn->id;
            WhoFacts     f;
            f.standing          = PPawn->loc.zone != nullptr && !PPawn->isDead() && !PPawn->isInEvent() && PPawn->status == xi::Status::Normal;
            f.inCity            = isCity(PPawn->loc.zone);
            f.inHisParty        = PPawn->PParty != nullptr && pawn::ordersOwnerOf(PPawn) != 0;
            const bool toHim    = f.inHisParty && pawn::travelMeetOf(id) == pawn::ordersOwnerOf(PPawn); // her way to him waits on her errands
            f.elsewhere         = pawn::errands::viewOf(id).has_value() || (pawn::travelOrderOf(id).has_value() && !toHim) || pawn::walkOrderOf(id).has_value() ||
                          pawn::world::isLeaving(id) || pawn::redress::isClaimed(id);
            f.worlds            = pawn::world::isBody(id) && pawn::world::inTheWild(id);
            const auto order    = pawn::world::townOrder(id);
            f.atHangout         = PPawn->PParty == nullptr && order.has_value() && order->atSeat && !order->leaving;
            f.seen              = f.standing && pawn::world::playerIn(static_cast<uint16>(PPawn->getZone()));
            return f;
        }

        auto busyOf(CCharEntity* PPawn) -> Busy
        {
            Busy b;
            b.fighting = PPawn->PAI != nullptr && PPawn->PAI->IsEngaged();
            if (auto* PController = controllerOf(PPawn); PController != nullptr)
            {
                using Mode      = CPawnController::Mode;
                const auto mode = PController->CurrentMode();
                b.fighting      = b.fighting || mode == Mode::Fight || mode == Mode::Hold || mode == Mode::Approach || mode == Mode::Attend ||
                             PController->PartyFightTarget() != nullptr || PController->PartyInFight();
                b.ordered = PController->HoldsOnOrder() || PController->HasPlayersOrder() || PController->InManeuver() || PController->HasQueuedOrder() ||
                            PController->QueueLine().lane.action.kind != CL_AK_NONE || PController->PlayersRestHolds() || PController->IsRetreating();
                b.acting = PController->Acting();
            }
            const uint32 by = pawn::walkOrderedBy(PPawn->id);
            b.ordered       = b.ordered || (by != 0 && by != pawn::kErrandWalker);
            b.resting       = PPawn->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Healing);
            return b;
        }

        auto whyBusy(const Busy& b) -> const char*
        {
            return b.fighting ? "a fight" : "the player's order";
        }

        // ---- where her stops are ----------------------------------------------------

        // Where a Mog House opens onto a zone: the ground its door sets a player
        // down on as he comes out, from the zone's own Mog House lines (zmr*,
        // zms*; the way out of one, zmrq, left aside). Read once a zone
        auto doorsOf(CZone* PZone) -> const std::vector<position_t>&
        {
            static std::unordered_map<uint16, std::vector<position_t>> known;
            const auto                                                 zone = static_cast<uint16>(PZone->GetID());
            if (const auto it = known.find(zone); it != known.end())
            {
                return it->second;
            }
            auto&      doors    = known[zone];
            const auto settings = xi::data::loadZoneFile<xi::data::datasets::zones::settings::Dataset>(PZone->GetID());
            if (!settings)
            {
                return doors;
            }
            for (const auto& line : settings->ZoneLines)
            {
                const auto letter = [&](const int i)
                {
                    return static_cast<char>((line.Id >> (8 * i)) & 0xFF);
                };
                const bool entrance = letter(0) == 'z' && letter(1) == 'm' && (letter(2) == 'r' || letter(2) == 's') && !(letter(2) == 'r' && letter(3) == 'q');
                if (entrance && line.DestinationZone == PZone->GetID())
                {
                    doors.push_back(line.Destination);
                }
            }
            return doors;
        }

        // The nearest Mog House door or Nomad Moogle of her zone
        auto mogStop(CCharEntity* PPawn) -> std::optional<StopAt>
        {
            std::optional<StopAt> best;
            float                 nearest  = 0.0f;
            const auto            consider = [&](const position_t& at)
            {
                const float away = distance(PPawn->loc.p, at);
                if (!best.has_value() || away < nearest)
                {
                    best    = StopAt{ Stop::MogHouse, at };
                    nearest = away;
                }
            };
            for (const auto& door : doorsOf(PPawn->loc.zone))
            {
                consider(door);
            }
            for (const auto* PNpc : PPawn->loc.zone->queryEntitiesByName("Nomad_Moogle"))
            {
                // the content gates and the cutscene-only moogles keep theirs hidden
                if (PNpc != nullptr && (PNpc->status == xi::Status::Normal || PNpc->status == xi::Status::Update))
                {
                    consider(PNpc->loc.p);
                }
            }
            return best;
        }

        // A gate guard of her zone who sells to her; else, in his party, her
        // own nation's consulate in another zone of her city. One of the
        // world's keeps to her own zone: her seat is there
        auto guardStop(CCharEntity* PPawn, const Who who) -> std::optional<StopAt>
        {
            const uint8 nation = PPawn->profile.nation;
            if (const auto local = pawn::guards::guardFor(PPawn->loc.zone, nation, PPawn->loc.p);
                local.has_value() && local->guard != nullptr && local->npc != nullptr && pawn::guards::sellsTo(*local->guard, nation))
            {
                return StopAt{ Stop::Guard, local->npc->loc.p, local->guard };
            }
            if (who != Who::Party)
            {
                return std::nullopt;
            }
            if (const auto consulate = pawn::guards::consulateFor(PPawn->loc.zone, nation); consulate.has_value())
            {
                return StopAt{ Stop::Consulate, PPawn->loc.p, consulate->guard, consulate->zone };
            }
            return std::nullopt;
        }

        // ---- her lines ----------------------------------------------------------------

        // His zone once he hears her: in a zone, for kHeardAfter
        auto hearsIn(const uint32 player) -> std::optional<uint16>
        {
            const auto* PPlayer = zoneutils::GetChar(player);
            if (PPlayer == nullptr || PPlayer->loc.zone == nullptr || pawn::isPawn(PPlayer))
            {
                heard.erase(player);
                return std::nullopt;
            }
            const auto zone = static_cast<uint16>(PPlayer->getZone());
            auto&      h    = heard[player];
            if (h.zone != zone || h.since == timer::time_point{})
            {
                h = Heard{ zone, timer::now() };
            }
            return timer::now() - h.since >= kHeardAfter ? std::optional<uint16>(zone) : std::nullopt;
        }

        // One of the kind's lines in her party's chat, to its real players
        void speak(const CCharEntity* PPawn, const Line line)
        {
            if (PPawn->PParty == nullptr)
            {
                return;
            }
            auto&      r   = recent[static_cast<std::size_t>(line)];
            const auto now = timer::now();
            if (now - r.at > kRecentLine)
            {
                r.mask = 0;
            }
            const auto index = cardian::town::pickLine(line, xirand::GetRandomNumber<uint32>(1000), r.mask);
            r.mask |= 1u << index;
            r.at            = now;
            const auto text = std::string(cardian::town::linesOf(line)[index]);
            for (auto* PMember : PPawn->PParty->members)
            {
                if (auto* PChar = dynamic_cast<CCharEntity*>(PMember); PChar != nullptr && !pawn::isPawn(PChar))
                {
                    PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PPawn, MESSAGE_PARTY, text);
                }
            }
            ShowInfoFmt("errand: {} says \"{}\"", PPawn->getName(), text);
        }

        void owe(const uint32 charid, const Line line, const uint32 player)
        {
            owed.push_back(Owed{ charid, line, player, timer::now() });
        }

        // Each owed line said once its player hears it, in order; one still in
        // his party no more, or unheard too long, let go
        void sayOwed()
        {
            const auto now = timer::now();
            for (auto it = owed.begin(); it != owed.end();)
            {
                const auto* PPawn = pawn::findPawn(it->charid);
                if (PPawn == nullptr || PPawn->PParty == nullptr || pawn::ordersOwnerOf(PPawn) != it->player || now - it->at > kOwedFor)
                {
                    it = owed.erase(it);
                    continue;
                }
                if (!hearsIn(it->player).has_value())
                {
                    ++it;
                    continue;
                }
                speak(PPawn, it->line);
                it = owed.erase(it);
            }
        }

        // ---- her walks ---------------------------------------------------------------

        void walk(Errand& e, const uint32 charid, const position_t& goal)
        {
            e.goal = goal;
            pawn::setWalkOrder(charid, goal, pawn::kErrandWalker);
        }

        // Her walk and her way to a consulate taken off her, if they are still hers
        void clearMine(const Errand& e, const uint32 charid)
        {
            if (e.goal.has_value() && pawn::walkOrderedBy(charid) == pawn::kErrandWalker)
            {
                if (const auto walking = pawn::walkOrderOf(charid); walking.has_value() && distance(*walking, *e.goal) < 0.1f)
                {
                    pawn::clearWalkOrder(charid);
                }
            }
            if (e.phase == Phase::Trek && e.next < e.stops.size() && pawn::travelOrderOf(charid) == static_cast<xi::ZoneId>(e.stops[e.next].zone))
            {
                pawn::clearTravelOrder(charid);
            }
        }

        // Over, every stop seen to: in his party she says so, once he hears it
        void ready(const uint32 charid, const CCharEntity* PPawn)
        {
            const auto it = errands.find(charid);
            if (it == errands.end())
            {
                return;
            }
            const Errand e = it->second;
            clearMine(e, charid);
            errands.erase(it);
            ShowInfoFmt("errand: {} is ready", PPawn->getName());
            if (e.who == Who::Party)
            {
                pawn::carryFromHere(PPawn); // a recruit from afar is still carried to him from where her errands left her
                owe(charid, Line::Ready, e.player);
            }
        }

        // A need she could not see to, kept from her looks for a while
        // wherever she goes, so two zones never send her back and forth
        void noteFailed(Watch& watch, const uint8 need)
        {
            watch.failed   = static_cast<uint8>((timer::now() - watch.failedAt < kRetryAfter ? watch.failed : 0) | need);
            watch.failedAt = timer::now();
        }

        // Ended for good -- she is no longer whose they were -- or, `suspended`,
        // until she is free in the city again, when she looks afresh
        void stop(const uint32 charid, const std::string& why, const bool suspended)
        {
            const auto it = errands.find(charid);
            if (it == errands.end())
            {
                return;
            }
            const bool party = it->second.who == Who::Party;
            clearMine(it->second, charid);
            errands.erase(it);
            ShowInfoFmt("errand: {} {} ({})", pawn::seats::nameOf(charid), suspended ? "stops her errands for now" : "drops her errands", why);
            if (party)
            {
                pawn::carryFromHere(pawn::findPawn(charid));
            }
            // Free again -- in the city, or as someone else's now: one of the
            // world's invited sets out afresh as his party member -- she looks
            auto& watch   = watches[charid];
            watch.free    = false;
            watch.pending = false;
        }

        // `from` turned round `at` by `turn` radians
        auto turned(const position_t& at, const position_t& from, const float turn) -> position_t
        {
            const float dx   = from.x - at.x;
            const float dz   = from.z - at.z;
            position_t  side = from;
            side.x           = at.x + dx * std::cos(turn) - dz * std::sin(turn);
            side.z           = at.z + dx * std::sin(turn) + dz * std::cos(turn);
            return side;
        }

        // Her side of a stop: where she comes from, turned about it by an angle
        // of her own, up to 30 degrees either way, so a crowd at one guard
        // stands spread round it rather than on one spot
        auto sideOf(const position_t& at, const position_t& from, const uint32 charid) -> position_t
        {
            return turned(at, from, (static_cast<float>(charid % 7) - 3.0f) * (std::numbers::pi_v<float> / 18.0f));
        }

        // Whether the walker finds her a route to `goal` from where she
        // stands: the search her walk order makes, on the same ground
        auto walkable(const CCharEntity* PPawn, const position_t& goal) -> bool
        {
            const float span = distance(PPawn->loc.p, goal);
            if (span < 1.0f)
            {
                return true;
            }
            const auto route = pawn::travel::routeAvoiding(PPawn->getZone(), PPawn->loc.p, goal, {}, {}, span * 3.0f + 100.0f);
            return route.has_value() && route->complete && !route->points.empty();
        }

        // Where she stands at a stop: beside it on her own side, turned by
        // her own angle, else the nearest other side the walker reaches from
        // where she stands, 30 degrees a step either way, all round -- a stop
        // in a corner has sides on ground no walk joins. The turn it took, in
        // degrees
        struct Beside
        {
            position_t at{};
            int        turnedBy = 0;
        };
        auto besideStop(const CCharEntity* PPawn, const position_t& stop) -> std::optional<Beside>
        {
            const auto              own = sideOf(stop, PPawn->loc.p, PPawn->id);
            std::vector<position_t> unwalkable; // sides that fell back to one floor are searched once
            for (int step = 0; step <= 6; ++step)
            {
                for (const int sign : { 1, -1 })
                {
                    if ((step == 0 || step == 6) && sign < 0)
                    {
                        continue; // her own side once, and the far side once
                    }
                    const int  degrees = sign * step * 30;
                    const auto side    = turned(stop, own, static_cast<float>(degrees) * std::numbers::pi_v<float> / 180.0f);
                    const auto goal    = pawn::errands::shortOf(PPawn->loc.zone, stop, side);
                    if (!goal.has_value() || std::ranges::any_of(unwalkable, [&](const position_t& p) { return distance(p, *goal) < 0.5f; }))
                    {
                        continue;
                    }
                    if (walkable(PPawn, *goal))
                    {
                        return Beside{ *goal, degrees };
                    }
                    unwalkable.push_back(*goal);
                }
            }
            return std::nullopt;
        }

        // A stop as the map log names it: a guard by his name
        auto stopWords(const StopAt& s) -> std::string
        {
            const auto words = std::string(cardian::town::stopName(s.stop));
            return s.guard != nullptr ? fmt::format("{} ({})", words, s.guard->name) : words;
        }

        void startStop(Errand& e, CCharEntity* PPawn);

        // On to her next stop; every stop done, one of the world's walks back
        // to her seat, unless the town has her leaving
        void nextStop(Errand& e, CCharEntity* PPawn)
        {
            ++e.next;
            if (e.next < e.stops.size())
            {
                startStop(e, PPawn);
                return;
            }
            if (e.who == Who::Hangout)
            {
                if (const auto order = pawn::world::townOrder(PPawn->id); order.has_value() && !order->leaving && distance(PPawn->loc.p, e.from) > kAtSeat)
                {
                    e.phase = Phase::Back;
                    e.since = timer::now();
                    walk(e, PPawn->id, e.from);
                    return;
                }
            }
            ready(PPawn->id, PPawn);
        }

        // A stop she cannot see to: nothing here, no way there, too long on the
        // way. Not looked for again in this zone
        void giveUp(Errand& e, CCharEntity* PPawn, const std::string& why)
        {
            const auto& s     = e.stops[e.next];
            auto&       watch = watches[PPawn->id];
            watch.seenTo |= cardian::town::needOf(s.stop);
            noteFailed(watch, cardian::town::needOf(s.stop));
            ShowInfoFmt("errand: {} gives up on {} at ({:.1f}, {:.1f}, {:.1f}), from ({:.1f}, {:.1f}, {:.1f}): {}", PPawn->getName(), stopWords(s), s.at.x, s.at.y, s.at.z,
                        PPawn->loc.p.x, PPawn->loc.p.y, PPawn->loc.p.z, why);
            nextStop(e, PPawn);
        }

        void startStop(Errand& e, CCharEntity* PPawn)
        {
            auto& s = e.stops[e.next];
            e.since = timer::now();
            e.asked = false;
            if (s.stop == Stop::Consulate && static_cast<uint16>(PPawn->getZone()) != s.zone)
            {
                clearMine(e, PPawn->id);
                e.goal.reset();
                if (!pawn::orderTravel(PPawn->id, s.zone, 0))
                {
                    giveUp(e, PPawn, fmt::format("zone {} is not loaded", s.zone));
                    return;
                }
                e.phase = Phase::Trek;
                ShowInfoFmt("errand: {} heads for her nation's consulate, {} in zone {}", PPawn->getName(), s.guard != nullptr ? s.guard->name : "?", s.zone);
                return;
            }
            if (s.stop == Stop::Consulate)
            {
                // in its zone: to its guard
                const auto npcs = s.guard != nullptr ? PPawn->loc.zone->queryEntitiesByName(std::string(s.guard->name)) : std::vector<CBaseEntity*>{};
                if (npcs.empty() || npcs.front() == nullptr)
                {
                    giveUp(e, PPawn, "her consulate's guard is not there");
                    return;
                }
                s.at = npcs.front()->loc.p;
            }
            const auto beside = besideStop(PPawn, s.at);
            if (!beside.has_value())
            {
                giveUp(e, PPawn, fmt::format("no floor beside ({:.1f}, {:.1f}, {:.1f}) the walker finds a route to", s.at.x, s.at.y, s.at.z));
                return;
            }
            if (beside->turnedBy != 0)
            {
                ShowInfoFmt("errand: {} stands at {} turned {} degrees from her own side, the walker finding no route there: ({:.1f}, {:.1f}, {:.1f})", PPawn->getName(),
                            stopWords(s), beside->turnedBy, beside->at.x, beside->at.y, beside->at.z);
            }
            e.phase = Phase::Walk;
            walk(e, PPawn->id, beside->at);
        }

        // At the stop: the census asked, her scrolls bought, her jobs changed
        void arrive(Errand& e, CCharEntity* PPawn)
        {
            const auto& s     = e.stops[e.next];
            const auto  name  = PPawn->getName();
            auto&       watch = watches[PPawn->id];
            if (s.stop != Stop::Guard && s.stop != Stop::Consulate)
            {
                watch.seenTo |= cardian::town::needOf(s.stop); // a guard's, only if she still lacks a scroll after it (below)
            }
            e.phase = Phase::At;
            e.since = timer::now();
            walk(e, PPawn->id, PPawn->loc.p); // she stands where she is while she is here
            switch (s.stop)
            {
                case Stop::Counter:
                    e.asked = pawn::redress::askFor(PPawn, e.afresh);
                    ShowInfoFmt("errand: {} at the counter{}", name, e.asked ? "; the census is asked to dress her" : ", dressed for her level already");
                    if (!e.asked)
                    {
                        nextStop(e, PPawn);
                    }
                    return;
                case Stop::Guard:
                case Stop::Consulate:
                {
                    if (s.guard == nullptr)
                    {
                        nextStop(e, PPawn);
                        return;
                    }
                    std::string  refusal;
                    const auto*  PTell  = e.who == Who::Party ? zoneutils::GetChar(e.player) : nullptr;
                    const uint32 bought = pawn::supplies::buyAt(PPawn, *s.guard, PTell, &refusal);
                    if (bought > 0)
                    {
                        ShowInfoFmt("errand: {} bought {} scroll{} at {}", name, bought, bought == 1 ? "" : "s", s.guard->name);
                    }
                    else
                    {
                        ShowInfoFmt("errand: {} bought nothing at {}{}", name, s.guard->name, refusal.empty() ? "" : fmt::format(" ({})", refusal));
                    }
                    // Another nation's guard turned her away: in his party, on to
                    // her own nation's consulate, whose stock is the same
                    const bool turnedAway = bought == 0 && (refusal == "OUTRANKED" || refusal == "FOREIGN_PLACE");
                    bool       onward     = false;
                    if (turnedAway && e.who == Who::Party && s.stop == Stop::Guard)
                    {
                        if (const auto consulate = pawn::guards::consulateFor(PPawn->loc.zone, PPawn->profile.nation); consulate.has_value())
                        {
                            e.stops.push_back(StopAt{ Stop::Consulate, PPawn->loc.p, consulate->guard, consulate->zone });
                            onward = true;
                        }
                    }
                    // Still lacking one, with nowhere further to go: a need she
                    // failed at. Bought, a scroll she uses later sends her again
                    if (!onward && pawn::supplies::wants(PPawn))
                    {
                        watch.seenTo |= cardian::town::kScrolls;
                        noteFailed(watch, cardian::town::kScrolls);
                    }
                    nextStop(e, PPawn);
                    return;
                }
                case Stop::MogHouse:
                    if (const auto jobs = pawn::redress::takeCareerJobs(PPawn); jobs.has_value())
                    {
                        e.afresh = true;
                        ShowInfoFmt("errand: {} changed job to {}", name, *jobs);
                    }
                    else
                    {
                        ShowInfoFmt("errand: {} at the Mog House: her jobs stand as they are", name);
                        if (e.jobsCounter)
                        {
                            // the counter was for the new jobs alone
                            for (std::size_t i = e.stops.size(); i-- > e.next + 1;)
                            {
                                if (e.stops[i].stop == Stop::Counter)
                                {
                                    e.stops.erase(e.stops.begin() + static_cast<std::ptrdiff_t>(i));
                                }
                            }
                        }
                    }
                    nextStop(e, PPawn);
                    return;
            }
        }

        // ---- setting out -------------------------------------------------------------

        void setOut(CCharEntity* PPawn, const Who who, Watch& watch)
        {
            const auto  now      = timer::now();
            const auto  joinedAt = std::exchange(watch.joinedAt, timer::time_point{});
            const bool  joined   = who == Who::Party && joinedAt != timer::time_point{} && now - joinedAt <= kJoinedFor;
            const bool  worlds   = pawn::seats::isWorlds(PPawn->id);
            const uint8 failed   = now - watch.failedAt < kRetryAfter ? watch.failed : 0;
            NeedFacts   facts{ .worlds = worlds, .guest = who == Who::Party, .lacksScrolls = pawn::supplies::wants(PPawn), .seenTo = static_cast<uint8>(watch.seenTo | failed) };
            if (worlds)
            {
                facts.risen      = pawn::redress::due(PPawn);
                facts.careerJobs = pawn::redress::careerWants(PPawn);
            }
            // Just invited, with nothing she can see to here, she says she is
            // ready (the user, 2026-10-17)
            const auto readyAsSheJoins = [&]
            {
                if (joined)
                {
                    owe(PPawn->id, Line::Ready, pawn::ordersOwnerOf(PPawn));
                }
            };
            const uint8 needs = cardian::town::needsOf(facts);
            if (needs == 0)
            {
                if (who == Who::Party)
                {
                    ShowInfoFmt("errand: {} needs nothing in {}{}", PPawn->getName(), zoneNameOf(PPawn), joined ? ", ready as she joins" : "");
                    readyAsSheJoins();
                }
                return;
            }

            std::vector<StopAt> found;
            std::vector<Spot>   spots;
            uint8               unmet = 0;
            const auto          add   = [&](const std::optional<StopAt>& s, const uint8 need)
            {
                if (!s.has_value())
                {
                    unmet |= need;
                    return;
                }
                found.push_back(*s);
                spots.push_back(Spot{ s->stop, s->at.x, s->at.y, s->at.z, s->stop == Stop::Consulate });
            };
            // The counter for her new jobs alone, when she has not risen: no
            // Mog House here, no counter
            const bool jobsOnly  = (needs & cardian::town::kJobs) != 0 && !facts.risen;
            const auto mog       = (needs & cardian::town::kJobs) != 0 ? mogStop(PPawn) : std::nullopt;
            const bool toCounter = (needs & cardian::town::kRedress) != 0 && !(jobsOnly && !mog.has_value());
            if (toCounter)
            {
                const auto* PCounter = pawn::errands::counterIn(PPawn);
                add(PCounter != nullptr ? std::optional<StopAt>(StopAt{ Stop::Counter, PCounter->loc.p }) : std::nullopt, cardian::town::kRedress);
            }
            if ((needs & cardian::town::kScrolls) != 0)
            {
                add(guardStop(PPawn, who), cardian::town::kScrolls);
            }
            if ((needs & cardian::town::kJobs) != 0)
            {
                add(mog, cardian::town::kJobs);
            }
            if (unmet != 0)
            {
                watch.seenTo |= unmet;
                ShowInfoFmt("errand: {} needs {}, and nothing in {} sees to it", PPawn->getName(), needsText(unmet), zoneNameOf(PPawn));
            }
            if (found.empty())
            {
                readyAsSheJoins();
                return;
            }

            Errand e;
            e.who         = who;
            e.player      = who == Who::Party ? pawn::ordersOwnerOf(PPawn) : 0;
            e.jobsCounter = jobsOnly;
            // Her way to him waits: done, she treks to him by the usual rule
            // from wherever her errands leave her
            const bool trekking = who == Who::Party && e.player != 0 && pawn::travelMeetOf(PPawn->id) == e.player;
            if (trekking)
            {
                pawn::clearTravelOrder(PPawn->id);
            }
            e.zone   = static_cast<uint16>(PPawn->getZone());
            e.from   = PPawn->loc.p;
            if (const auto* PPlayer = pawn::partyPlayer(PPawn); PPlayer != nullptr && PPlayer->loc.zone != nullptr)
            {
                e.hisZone = static_cast<uint16>(PPlayer->getZone());
            }
            for (const auto i : cardian::town::visitOrder(spots, PPawn->loc.p.x, PPawn->loc.p.y, PPawn->loc.p.z))
            {
                e.stops.push_back(found[i]);
            }
            std::string route;
            for (const auto& s : e.stops)
            {
                route += route.empty() ? "" : ", then ";
                route += cardian::town::stopName(s.stop);
            }
            ShowInfoFmt("errand: {} sets out ({}) in {}{}{}: {}", PPawn->getName(), needsText(needs & ~unmet), zoneNameOf(PPawn),
                        watch.setOutHere ? ", taken up again" : "", trekking ? ", her way to the player after" : "", route);
            if (cardian::town::saysSetOut(who, watch.setOutHere))
            {
                owe(PPawn->id, Line::SetsOut, e.player);
            }
            watch.setOutHere = true;
            auto& kept       = errands[PPawn->id];
            kept             = std::move(e);
            startStop(kept, PPawn);
        }

        // ---- the errands under way --------------------------------------------------

        void step(const uint32 charid)
        {
            const auto it = errands.find(charid);
            if (it == errands.end())
            {
                return;
            }
            auto& e     = it->second;
            auto* PPawn = pawn::findPawn(charid);
            if (PPawn == nullptr)
            {
                stop(charid, "her body is gone", false);
                return;
            }
            if (PPawn->loc.zone == nullptr)
            {
                return; // between two zones of her city on her way: she lands in a moment
            }
            const uint32 id       = PPawn->id;
            const auto   zone     = static_cast<uint16>(PPawn->getZone());
            const auto*  s        = e.next < e.stops.size() ? &e.stops[e.next] : nullptr;
            const bool   consular = s != nullptr && s->stop == Stop::Consulate;
            OnFacts      on;
            on.standing   = !PPawn->isDead() && !PPawn->isInEvent();
            on.inHisParty = PPawn->PParty != nullptr && pawn::ordersOwnerOf(PPawn) == e.player;
            on.worlds     = pawn::world::isBody(id) && pawn::world::inTheWild(id);
            on.sameZone   = zone == e.zone;
            on.onTheWay   = consular && (e.phase == Phase::Trek || zone == s->zone);
            if (!cardian::town::goesOn(e.who, on))
            {
                stop(charid, !on.standing ? "she is down" : !(on.sameZone || on.onTheWay) ? "she has left her zone" : "she is no longer whose they were", false);
                return;
            }
            const auto busy     = busyOf(PPawn);
            const auto walking  = pawn::walkOrderOf(id);
            const bool replaced = walking.has_value() && (pawn::walkOrderedBy(id) != pawn::kErrandWalker || !e.goal.has_value() || distance(*walking, *e.goal) > 0.1f);
            if (cardian::town::interrupted(busy) || replaced)
            {
                stop(charid, replaced && !cardian::town::interrupted(busy) ? "another walk took her" : whyBusy(busy), true);
                return;
            }

            // His going on ends nothing: she says so once he hears it, in a zone
            // neither hers nor the one he stood in as she set out (between two
            // zones as she set out, he stood in the first he is heard in)
            if (e.who == Who::Party)
            {
                const auto his = hearsIn(e.player);
                if (his.has_value() && e.hisZone == 0)
                {
                    e.hisZone = *his;
                }
                const bool heLeft = his.has_value() && *his != zone && *his != e.hisZone;
                if (cardian::town::saysNotReady(e.who, heLeft, his.has_value(), e.saidNotReady))
                {
                    e.saidNotReady = true;
                    ShowInfoFmt("errand: {} keeps on with her errands ({} has left for zone {})", PPawn->getName(), pawn::seats::nameOf(e.player), *his);
                    speak(PPawn, Line::NotReady);
                }
            }

            if (s == nullptr && e.phase != Phase::Back)
            {
                ready(charid, PPawn);
                return;
            }
            const auto now = timer::now();
            switch (e.phase)
            {
                case Phase::Walk:
                {
                    const bool atGoal = e.goal.has_value() && distance(PPawn->loc.p, *e.goal) <= kAtGoal;
                    if ((distance(PPawn->loc.p, s->at) <= kAtStop || atGoal) && !busy.acting) // a cast or an item's use ends first
                    {
                        arrive(e, PPawn);
                        return;
                    }
                    if (!walking.has_value())
                    {
                        giveUp(e, PPawn, e.goal.has_value() ? fmt::format("her walk to ({:.1f}, {:.1f}, {:.1f}) was dropped", e.goal->x, e.goal->y, e.goal->z)
                                                            : std::string("her walk was dropped"));
                        return;
                    }
                    if (now - e.since >= kWalkFor)
                    {
                        giveUp(e, PPawn, "too long on the way");
                    }
                    return;
                }
                case Phase::Trek:
                {
                    if (pawn::travelMeetOf(id) != 0)
                    {
                        pawn::orderTravel(id, s->zone, 0); // his Follow came over her way there: it waits on her errands
                    }
                    if (!pawn::travelOrderOf(id).has_value())
                    {
                        if (zone == s->zone)
                        {
                            startStop(e, PPawn); // in its zone: to its guard
                            return;
                        }
                        giveUp(e, PPawn, "her way there ended short of it");
                        return;
                    }
                    if (now - e.since >= kTrekFor)
                    {
                        pawn::clearTravelOrder(id);
                        giveUp(e, PPawn, "too long on the way");
                    }
                    return;
                }
                case Phase::At:
                {
                    if (!e.asked || pawn::redress::settled(id) || now - e.since >= kCensusWait)
                    {
                        if (e.asked)
                        {
                            ShowInfoFmt("errand: {} {}", PPawn->getName(), pawn::redress::settled(id) ? "is dressed at the counter" : "waited long enough for the census");
                        }
                        nextStop(e, PPawn);
                        return;
                    }
                    if (!walking.has_value() && e.goal.has_value())
                    {
                        walk(e, id, *e.goal); // the walker let her stand go: given again
                    }
                    return;
                }
                case Phase::Back:
                {
                    const bool back = distance(PPawn->loc.p, e.from) <= kAtSeat;
                    if (back || !walking.has_value() || now - e.since >= kWalkFor)
                    {
                        if (const auto order = pawn::world::townOrder(id); order.has_value() && order->face.has_value())
                        {
                            PPawn->loc.p.rotation = worldAngle(PPawn->loc.p, *order->face);
                            PPawn->updatemask |= UPDATE_POS;
                        }
                        ShowInfoFmt("errand: {} {}", PPawn->getName(), back ? "is back at her seat" : "stops short of her seat");
                        ready(charid, PPawn);
                    }
                    return;
                }
            }
        }

        // ---- the look ----------------------------------------------------------------

        // Every cardian of a zone: on her errands, kept free; else she looks at
        // her needs as she arrives in a city's zone and as she becomes free
        // there, once nothing she is in the middle of keeps her
        void look(CZone* PZone)
        {
            const auto zone = static_cast<uint16>(PZone->GetID());
            const bool city = isCity(PZone);
            std::vector<CCharEntity*> here;
            PZone->ForEachChar([&](CCharEntity* PChar)
            {
                if (PChar != nullptr && pawn::isPawn(PChar))
                {
                    here.push_back(PChar);
                }
            });
            for (auto* PPawn : here)
            {
                auto&      watch   = watches[PPawn->id];
                const bool arrived = watch.zone != zone;
                if (arrived)
                {
                    watch.seenTo     = 0;
                    watch.setOutHere = false;
                }
                // A scroll gone from her bags since the last look -- used, by his
                // order or her own -- is a need come to her here: looked for
                // again, though she has shopped in this zone, and owed a look
                const uint8 lacked = pawn::supplies::lacks(PPawn);
                const bool  used   = (lacked & ~watch.lacked) != 0;
                watch.lacked       = lacked;
                if (used)
                {
                    watch.seenTo &= static_cast<uint8>(~cardian::town::kScrolls);
                }
                if (errands.contains(PPawn->id))
                {
                    watch.zone    = zone;
                    watch.free    = true;
                    watch.pending = false;
                    continue;
                }
                const auto who  = city ? cardian::town::whoGoes(whoOf(PPawn)) : Who::Nobody;
                const auto busy = who != Who::Nobody ? busyOf(PPawn) : Busy{};
                const bool free = who != Who::Nobody && !cardian::town::interrupted(busy);
                if (cardian::town::looksNow({ watch.zone, watch.free, watch.who }, zone, free, who))
                {
                    // a party member at once; one of the world's in a while
                    watch.pending   = true;
                    watch.notBefore = who == Who::Hangout ? timer::now() + std::chrono::seconds(xirand::GetRandomNumber(kStaggerMin, kStaggerMax + 1)) : timer::now();
                }
                if (who == Who::Party && watch.who != Who::Party)
                {
                    watch.setOutHere = false; // his now: she says she is getting ready, as his
                }
                watch.zone = zone;
                watch.free = free;
                watch.who  = who;
                if (!free)
                {
                    watch.pending = false;
                    continue;
                }
                if (used && !watch.pending)
                {
                    watch.pending   = true;
                    watch.notBefore = timer::now();
                }
                if (watch.pending && timer::now() >= watch.notBefore && cardian::town::setsOutNow(busy, who))
                {
                    watch.pending = false;
                    setOut(PPawn, who, watch);
                }
            }
        }

        // The errands under way, and the lines owed; a watch on a body gone, let go
        void stepAll()
        {
            std::vector<uint32> charids;
            charids.reserve(errands.size());
            for (const auto& [charid, e] : errands)
            {
                charids.push_back(charid);
            }
            for (const auto charid : charids)
            {
                step(charid);
            }
            sayOwed();
            std::erase_if(watches, [](const auto& entry)
            {
                return pawn::findPawn(entry.first) == nullptr;
            });
        }
    } // namespace

    void tick(CZone* PZone)
    {
        if (!settings::get<bool>("pawn.TOWN_ERRANDS"))
        {
            return;
        }
        const auto now = timer::now();
        if (now - steppedAt >= kLookEvery)
        {
            steppedAt = now;
            stepAll();
        }
        if (PZone == nullptr)
        {
            return;
        }
        auto& looked = lookedAt[static_cast<uint16>(PZone->GetID())];
        if (now - looked >= kLookEvery)
        {
            looked = now;
            look(PZone);
        }
    }

    auto busy(const uint32 charid) -> bool
    {
        if (errands.contains(charid))
        {
            return true;
        }
        // Just invited in a city: her first look comes before any trek to him
        const auto it = watches.find(charid);
        if (it == watches.end() || it->second.joinedAt == timer::time_point{} || timer::now() - it->second.joinedAt > kJoinedFor)
        {
            return false;
        }
        const auto* PPawn = pawn::findPawn(charid);
        return PPawn != nullptr && isCity(PPawn->loc.zone);
    }

    void noteJoined(const uint32 charid)
    {
        watches[charid].joinedAt = timer::now();
    }
} // namespace pawn::town
