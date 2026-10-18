// Cardian: a cardian on a chocobo (ROADMAP T, "Chocobos on the trek"; the
// rules pawn/riding.h applies, pure so the tests can hold them): who holds
// the Chocobo License by her own levels, who comes along when the player
// rents at a stable, and when a rider gets down.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace cardian::riding
{
    // A body of the world holds the license once any of her jobs stands here
    // or above, and rides on any of her jobs then, a fresh one at 1 included
    constexpr uint8_t kLicenseLevel = 20;

    // Beside the player at the stable as he rents: rents with him
    constexpr float kBesideYalms = 20.0f;

    // Reached the player once he is on foot: gets down
    constexpr float kReachedYalms = 10.0f;

    // The license by her own levels: any job at kLicenseLevel or above
    inline auto licensedByJobs(const std::span<const uint8_t> levels) -> bool
    {
        return std::ranges::any_of(levels, [](const uint8_t level) { return level >= kLicenseLevel; });
    }

    // A rider gets down in a fight, and once the player is on foot and she
    // has reached him; she rides on while he rides, and while she is still
    // on her way to him
    inline auto dismounts(const bool engaged, const bool playerRides, const bool reachedPlayer) -> bool
    {
        return engaged || (!playerRides && reachedPlayer);
    }
} // namespace cardian::riding
