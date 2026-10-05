// Cardian: a cardian's hold position as the player orders it and as a warp
// sets it (CPawnController::SetWaiting, HoldForWarp). One rule for every warp,
// both ways -- the player warped away from her (an outpost warp, Warp, a
// Teleport spell, a home point), or she carried away from him (the party's
// warp, a warp of her own, her home point): parted by magic, she holds where
// she is, and the hold lifts by itself once they are in one zone again. A
// hold the player ordered is his: a warp leaves it as it is, and nothing but
// his word lifts it. Treks are for walks: a zone change his client walked
// through a zone line is followed as ever.
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
} // namespace cardian::hold
