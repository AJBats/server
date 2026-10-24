// Cardian: a cardian's hold position as the player orders it and as a warp
// sets it (CPawnController::SetWaiting, HoldForWarp). One rule for every warp,
// both ways -- the player warped away from her (Warp, a Teleport spell, a
// ring), or she carried away from him (the party's warp, a warp of her own,
// her home point): parted by magic, she holds where she is, and the hold
// lifts by itself once they are in one zone again. A hold the player ordered
// is his: a warp leaves it as it is, and nothing but his word lifts it.
// Treks are for walks: a zone change his client walked through a zone line,
// or one a scene's end makes (movedByScene), is followed as ever. And in his
// zone, a walk to him that only the long way round makes holds her too, and
// nothing but his word lifts it, which sends her round (the user,
// 2026-10-24).
#pragma once

#include <chrono>
#include <optional>

namespace cardian::hold
{
    struct Hold
    {
        bool on      = false;
        bool ordered = false; // the player's own order: lifted only by him
        bool warp    = false; // the automatic hold a warp set: lifted when they are in one zone again
        bool longWay = false; // the automatic hold a long way round set: lifted by his word alone, which sends her round

        auto operator==(const Hold&) const -> bool = default;
    };

    // The player's order to hold, or to follow: his own
    inline auto ordered(const bool on) -> Hold
    {
        return { on, on, false };
    }

    // A warp parts them: she holds where she is, unless she holds on his
    // order already
    inline auto warped(const Hold& now) -> Hold
    {
        return now.on && now.ordered ? now : Hold{ true, false, true };
    }

    // Her walk to him is made only the long way round: she holds where she
    // is, unless she holds already
    inline auto longWayRound(const Hold& now) -> Hold
    {
        return now.on ? now : Hold{ true, false, false, true };
    }

    // In one zone with him again: the automatic hold lifts by itself; a hold
    // he ordered stands
    inline auto reunited(const Hold& now) -> Hold
    {
        return now.on && now.warp ? Hold{} : now;
    }

    // His zone change is a walk when his client walked into a zone line for
    // it moments before (the zone line packet); any other is magic. The window
    // covers the destination zone getting ready to take him
    constexpr auto kZoneLineWindow = std::chrono::seconds(15);

    inline auto walked(const std::optional<std::chrono::milliseconds> sinceZoneLine) -> bool
    {
        return sinceZoneLine.has_value() && sinceZoneLine->count() >= 0 && *sinceZoneLine <= kZoneLineWindow;
    }

    // A zone change a scene's end makes is followed too, through where he
    // stood (the user, 2026-10-22: bring the party along, and let play say
    // otherwise): a gate's trigger area, a door's NPC, a portal, a home point,
    // a guide (pawn::noteEventEnd). Most moves follow the end within a tick
    // or two; an outpost's teleporter or a conquest vendor's warp moves him
    // when the TELEPORT effect its finish gave him wears off, a duration of 1
    // or 3 seconds read on the zone's 3-second effect tick, so up to 6 seconds
    // after. The window takes those in, and no warp of his starts inside it
    constexpr auto kSceneWindow = std::chrono::seconds(7);

    inline auto movedByScene(const std::optional<std::chrono::milliseconds> sinceSceneEnd) -> bool
    {
        return sinceSceneEnd.has_value() && sinceSceneEnd->count() >= 0 && *sinceSceneEnd <= kSceneWindow;
    }
} // namespace cardian::hold
