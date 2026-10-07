// Cardian: where an idle cardian's eyes go, when she emotes, and which way a
// mage faces at her safety spot (ROADMAP G, items 1 and 4; #249). Pure
// arithmetic on seconds and dice, so the timing rules are tested without a
// map: the controller (CPawnController::IdleLook, IdleEmote,
// FaceBattleOnArrival) supplies the clock, the dice and the settings.
//
// Out of a fight she looks ahead. Now and then she glances at the player for
// a few seconds, and she looks at whoever she emotes at. Her emotes are rare,
// counted from her last one, never in a fight, and aimed at nobody, at the
// player or at another party member. In a fight her eyes are on it; once it
// is over the glances start afresh and an emote that came due waits a short
// random while, so a fight's end never has the whole party turn or emote as
// one.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cardian::glance
{
    // A stretch of seconds, drawn evenly between its two ends; `roll` is a
    // die in [0, 1). A negative end counts as zero
    struct Span
    {
        double min = 0.0;
        double max = 0.0;
    };

    inline auto draw(const Span span, const double roll) -> double
    {
        const double lo = std::max(0.0, std::min(span.min, span.max));
        const double hi = std::max(0.0, std::max(span.min, span.max));
        return lo + (hi - lo) * std::clamp(roll, 0.0, 1.0);
    }

    // A fight anywhere in her party holds her glances and her emotes: her
    // own (drawn, holding, attending, walking in on a mob), a party member
    // engaged, or a mob on the party -- engaged on a member, or claimed by one
    struct Fight
    {
        bool own           = false;
        bool memberEngaged = false;
        bool mobOnParty    = false;
    };

    inline auto inFight(const Fight& fight) -> bool
    {
        return fight.own || fight.memberEngaged || fight.mobOnParty;
    }

    // The glance at the player: one every `gap` seconds, lasting `length`
    struct GlanceTiming
    {
        Span gap;
        Span length;
    };

    struct Glances
    {
        double nextAt = -1.0; // the next glance is due; negative: the clock has not started
        double until  = -1.0; // the glance under way ends
    };

    // One tick of the glance clock at `now`: whether she looks at the player
    // this tick. The first tick only starts the clock, so nobody glances the
    // moment she arrives and no two members glance in step. `inSight` (the
    // player within glance range) lets a due glance go; out of sight, a
    // glance under way ends and a due one is let go, the next drawn. A `gap` whose max is
    // zero turns the glances off. `roll` is called once for each die it needs
    template <typename Roll>
    auto step(Glances& clock, const double now, const bool inSight, const GlanceTiming& timing, Roll&& roll) -> bool
    {
        if (timing.gap.max <= 0.0)
        {
            clock.until = -1.0;
            return false;
        }
        if (clock.nextAt < 0.0)
        {
            clock.nextAt = now + draw(timing.gap, roll());
            return false;
        }
        if (!inSight)
        {
            clock.until = -1.0;
            if (now >= clock.nextAt)
            {
                clock.nextAt = now + draw(timing.gap, roll());
            }
            return false;
        }
        if (now < clock.until)
        {
            return true;
        }
        if (now >= clock.nextAt)
        {
            clock.until  = now + draw(timing.length, roll());
            clock.nextAt = clock.until + draw(timing.gap, roll());
            return true;
        }
        return false;
    }

    // The emote clock: one emote every `gap` seconds, counted from the last
    struct Emotes
    {
        double nextAt = -1.0; // the next emote is due; negative: the clock has not started
    };

    // Whether an emote is due at `now`. The first call only starts the
    // clock: no emote the moment she arrives. A due emote stays due until it
    // goes or is let go (rearm): a cast, a kneel or a fight it waits out; a
    // walk lets it go, so none piles up for the moment the party stops. A
    // `gap` whose max is zero turns the emotes off
    template <typename Roll>
    auto due(Emotes& clock, const double now, const Span gap, Roll&& roll) -> bool
    {
        if (gap.max <= 0.0)
        {
            return false;
        }
        if (clock.nextAt < 0.0)
        {
            clock.nextAt = now + draw(gap, roll());
            return false;
        }
        return now >= clock.nextAt;
    }

    // An emote gone, or let go: the next one a gap from now
    template <typename Roll>
    void rearm(Emotes& clock, const double now, const Span gap, Roll&& roll)
    {
        clock.nextAt = now + draw(gap, roll());
    }

    // Back from a fight, or from a stretch away from standing about: an
    // emote due by now, or about to be, waits `stagger` seconds, drawn, so
    // the members of a party never emote as one when the fight ends. One due
    // later keeps its time: the gap is not started over
    template <typename Roll>
    void settle(Emotes& clock, const double now, const Span stagger, Roll&& roll)
    {
        if (clock.nextAt < 0.0)
        {
            return;
        }
        clock.nextAt = std::max(clock.nextAt, now + draw(stagger, roll()));
    }

    // Whom an emote is aimed at
    enum class Aim : uint8_t
    {
        Nobody, // a fidget of her own: think, sigh, look about
        Player,
        Member, // another party member near her
    };

    // The aim of an emote: the player `atPlayer` percent of the time,
    // another member `atMember` percent, else nobody. An aim with nobody to
    // take it -- the player away, no member near her -- is a fidget of her own
    inline auto aim(const double roll, const double atPlayer, const double atMember, const bool hasPlayer, const bool hasMember) -> Aim
    {
        const double percent = std::clamp(roll, 0.0, 1.0) * 100.0;
        const double player  = std::max(0.0, atPlayer);
        const double member  = std::max(0.0, atMember);
        if (percent < player)
        {
            return hasPlayer ? Aim::Player : Aim::Nobody;
        }
        if (percent < player + member)
        {
            return hasMember ? Aim::Member : Aim::Nobody;
        }
        return Aim::Nobody;
    }

    // Headings are the game's: 256 to the turn
    constexpr int kTurn = 256;

    // The signed turn from heading `from` to heading `to`, the short way
    // round: -128 to 127
    inline auto turnBetween(const uint8_t from, const uint8_t to) -> int
    {
        const int turn = (static_cast<int>(to) - static_cast<int>(from) + kTurn) % kTurn;
        return turn >= kTurn / 2 ? turn - kTurn : turn;
    }

    // The heading a mage takes at her safety spot: facing the battle
    // (`toward`, the heading from her to the mob), give or take up to
    // `arcDegrees` either side, drawn by `roll`, so a party's mages never
    // stand in step. The arc is held to a half turn
    inline auto battleHeading(const uint8_t toward, const double arcDegrees, const double roll) -> uint8_t
    {
        const double arc  = std::clamp(arcDegrees, 0.0, 180.0) * kTurn / 360.0;
        const double off  = (std::clamp(roll, 0.0, 1.0) * 2.0 - 1.0) * arc;
        const int    turn = static_cast<int>(std::lround(off));
        return static_cast<uint8_t>(((static_cast<int>(toward) + turn) % kTurn + kTurn) % kTurn);
    }
} // namespace cardian::glance
