/*
===========================================================================

  Cardian: the seat waterfall's adapter (ROADMAP H). See seats.h.

===========================================================================
*/

#include "seats.h"

#include "seat_ladder.h"

#include "pawn.h"
#include "world.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/timer.h"

#include "entities/char_entity.h"
#include "utils/charutils.h"
#include "utils/zoneutils.h"

#include <chrono>
#include <optional>
#include <unordered_map>

namespace
{
    using namespace pawn::seats;

    std::optional<Ladder> ladder;

    // A summon: the player to stand her beside, consumed by the next stand
    std::unordered_map<uint32, uint32> summonBeside;

    // An invite on its way to her: for kInviteHold she ranks as a party
    // member, so the stand the invite needs is not refused for bodies that
    // outrank her tier -- and, newest, she outranks an older party mate of
    // her tier when the cap is full of them. Cleared when she joins (the
    // party carries her from there), when the stand is refused, and when
    // she leaves the ladder; a declined or lapsed invite runs it out
    std::unordered_map<uint32, timer::time_point> invited;
    constexpr auto                                kInviteHold = std::chrono::seconds(60);

    auto invitedNow(const uint32 charid) -> bool
    {
        const auto it = invited.find(charid);
        if (it == invited.end())
        {
            return false;
        }
        if (timer::now() - it->second >= kInviteHold)
        {
            invited.erase(it);
            return false;
        }
        return true;
    }

    // Live overrides for the two caps, 0 = the setting (debug: !pawnworld cap)
    uint32 standingOverride = 0;
    uint32 fadedOverride    = 0;

    timer::time_point lastRun{};
    constexpr auto    kRunEvery = std::chrono::milliseconds(500);

    auto standingCap() -> uint32
    {
        return standingOverride != 0 ? standingOverride : settings::get<uint32>("pawn.WORLD_STANDING_CAP");
    }

    auto fadedCap() -> uint32
    {
        return fadedOverride != 0 ? fadedOverride : settings::get<uint32>("pawn.WORLD_FADED_CAP");
    }

    // Under a player: a cardian_pawns row is a recruit, none is her own alt
    auto ownedTier(const uint32 charid) -> Tier
    {
        const auto rset = db::preparedStmt("SELECT pawn_charid FROM cardian_pawns WHERE pawn_charid = ?", charid);
        return rset && rset->next() ? Tier::Owned : Tier::Alt;
    }

    // The world's own: a memory row says the player has partied with her
    auto worldTier(const uint32 charid) -> Tier
    {
        const auto rset = db::preparedStmt("SELECT pawn_charid FROM cardian_party_memory WHERE pawn_charid = ? AND last_partied IS NOT NULL", charid);
        return rset && rset->next() ? Tier::Partied : Tier::Crowd;
    }

    auto nameOf(const uint32 charid) -> std::string
    {
        const auto rset = db::preparedStmt("SELECT charname FROM chars WHERE charid = ?", charid);
        return rset && rset->next() ? rset->get<std::string>("charname") : fmt::format("#{}", charid);
    }

    // ---- the engine: the only place a cardian is stood, faded or signed out --

    bool stand(const uint32 charid)
    {
        const auto* e = ladder->entryOf(charid);
        if (e == nullptr)
        {
            return false;
        }
        if (e->facts.owner == 0)
        {
            return pawn::world::standBody(charid);
        }
        if (const auto it = summonBeside.find(charid); it != summonBeside.end())
        {
            const uint32 player = it->second;
            summonBeside.erase(it);
            if (auto* PPlayer = zoneutils::GetChar(player); PPlayer != nullptr && pawn::standBeside(charid, PPlayer))
            {
                return true;
            }
        }
        return pawn::standOwned(charid, e->facts.owner);
    }

    void fade(const uint32 charid)
    {
        const auto* e = ladder->entryOf(charid);
        if (e != nullptr && e->facts.owner == 0)
        {
            pawn::world::fadeBody(charid);
        }
        else
        {
            pawn::despawnById(charid, true);
        }
    }

    void signIn(const uint32 charid)
    {
        const auto* e = ladder->entryOf(charid);
        if (e != nullptr && e->facts.owner == 0)
        {
            pawn::world::signInBody(charid);
        }
        else
        {
            pawn::markOnline(charid);
        }
    }

    void signOut(const uint32 charid)
    {
        pawn::markAbsent(charid);
    }

    auto factsOf(const uint32 charid) -> std::optional<Facts>
    {
        const auto* e = ladder ? ladder->entryOf(charid) : nullptr;
        return e != nullptr ? std::optional(e->facts) : std::nullopt;
    }
} // namespace

namespace pawn::seats
{
    void init()
    {
        ladder.emplace(Ladder::defaultOrder,
                       Lookup{
                           [](const uint16 zone) { return pawn::world::zoneWarm(zone); },
                           [](const uint16 zone) { return pawn::world::playerIn(zone); },
                           [](const uint32 charid) { return invitedNow(charid) || pawn::withRealPlayer(charid); },
                       },
                       Engine{ stand, fade, signIn, signOut });
        summonBeside.clear();
        invited.clear();
        lastRun = {};
    }

    void offerWorld(const uint32 charid, const uint16 zone)
    {
        if (ladder && charid != 0)
        {
            ladder->offer(charid, Facts{ .zone = zone, .tier = worldTier(charid), .owner = 0 });
        }
    }

    void offerOwned(const uint32 charid, const uint32 ownerCharID, const uint16 zone)
    {
        if (ladder && charid != 0 && ownerCharID != 0)
        {
            ladder->offer(charid, Facts{ .zone = zone, .tier = ownedTier(charid), .owner = ownerCharID });
        }
    }

    void setDown(const uint32 charid, const bool down)
    {
        if (auto facts = factsOf(charid); facts.has_value() && facts->down != down)
        {
            facts->down = down;
            ladder->offer(charid, *facts);
        }
    }

    void moved(const uint32 charid, const uint16 zone)
    {
        if (auto facts = factsOf(charid); facts.has_value() && facts->zone != zone)
        {
            facts->zone = zone;
            ladder->offer(charid, *facts);
        }
    }

    void touch(const uint32 charid)
    {
        if (ladder)
        {
            ladder->touch(charid);
        }
    }

    void withdraw(const uint32 charid)
    {
        if (ladder)
        {
            summonBeside.erase(charid);
            invited.erase(charid);
            ladder->withdraw(charid);
        }
    }

    auto ownedBy(const uint32 ownerCharID) -> std::vector<uint32>
    {
        return ladder && ownerCharID != 0 ? ladder->ownedBy(ownerCharID) : std::vector<uint32>{};
    }

    bool has(const uint32 charid)
    {
        return ladder && ladder->entryOf(charid) != nullptr;
    }

    void notePartied(const uint32 playerCharID, const uint32 pawnCharID)
    {
        if (!ladder || playerCharID == 0 || pawnCharID == 0)
        {
            return;
        }
        db::preparedStmt("INSERT INTO cardian_party_memory (player_charid, pawn_charid, last_partied) VALUES (?, ?, NOW()) "
                         "ON DUPLICATE KEY UPDATE last_partied = NOW()",
                         playerCharID, pawnCharID);
        invited.erase(pawnCharID);
        if (auto facts = factsOf(pawnCharID); facts.has_value())
        {
            if (facts->owner == 0 && facts->tier < Tier::Partied)
            {
                facts->tier = Tier::Partied;
                ladder->offer(pawnCharID, *facts);
            }
            ladder->touch(pawnCharID);
        }
    }

    void summon(const uint32 charid, const uint32 playerCharID)
    {
        if (charid != 0 && playerCharID != 0)
        {
            summonBeside[charid] = playerCharID;
        }
    }

    void tick()
    {
        const auto now = timer::now();
        if (now - lastRun < kRunEvery)
        {
            return;
        }
        run();
    }

    void run()
    {
        if (!ladder)
        {
            return;
        }
        lastRun = timer::now();
        ladder->setCaps(standingCap(), fadedCap());
        ladder->run(timer::now());
    }

    bool isStanding(const uint32 charid)
    {
        return ladder && ladder->stepOf(charid) == Step::Standing;
    }

    void setCaps(const uint32 standing, const uint32 faded)
    {
        standingOverride = standing;
        fadedOverride    = faded;
    }

    auto capsLine(const uint16 zoneId) -> std::string
    {
        if (!ladder)
        {
            return "the ladder is not built";
        }
        std::string spread;
        uint32      hereStanding = 0;
        uint32      hereOnline   = 0;
        for (const auto& [zone, counts] : ladder->zoneCounts())
        {
            const auto& [stood, online] = counts;
            if (stood > 0)
            {
                spread += fmt::format("{}{}:{}", spread.empty() ? "" : " ", zone, stood);
            }
            if (zone == zoneId)
            {
                hereStanding = stood;
                hereOnline   = online;
            }
        }
        return fmt::format("standing {}/{} server-wide{}{}; zone {}: {}/{} online, {} standing; {} in the ladder",
                           ladder->standing(), standingCap(), standingOverride != 0 ? " (set)" : "",
                           spread.empty() ? "" : fmt::format(" [{}]", spread),
                           zoneId, hereOnline, fadedCap(), hereStanding, ladder->size());
    }

    auto fadedNames(const uint32 ownerCharID) -> std::vector<std::string>
    {
        std::vector<std::string> names;
        if (ladder)
        {
            for (const uint32 charid : ladder->belowTheLine(ownerCharID))
            {
                names.push_back(nameOf(charid));
            }
        }
        return names;
    }

    auto recall(const uint32 ownerCharID, const std::string& name) -> std::string
    {
        const uint32 charid = charutils::getCharIdFromName(name);
        const auto   facts  = factsOf(charid);
        if (!facts.has_value() || facts->owner != ownerCharID)
        {
            return fmt::format("{} is not one of yours in the waterfall", name);
        }
        if (isStanding(charid))
        {
            return fmt::format("{} is already standing", name);
        }
        ladder->touch(charid);
        run();
        return isStanding(charid) ? std::string{} : fmt::format("{} is at the front of her tier, but the caps have no room yet", name);
    }

    auto nameOf(const uint32 charid) -> std::string
    {
        return ::nameOf(charid);
    }

    bool heldFor(const uint32 charid, const uint32 playerCharID)
    {
        const auto facts = factsOf(charid);
        return facts.has_value() && (facts->owner == 0 || facts->owner == playerCharID);
    }

    bool isWorlds(const uint32 charid)
    {
        const auto facts = factsOf(charid);
        return facts.has_value() && facts->owner == 0;
    }

    bool inviteStand(const uint32 charid)
    {
        if (!has(charid))
        {
            return false;
        }
        if (isStanding(charid))
        {
            return true;
        }
        invited[charid] = timer::now();
        ladder->touch(charid);
        run();
        if (!isStanding(charid))
        {
            invited.erase(charid);
            return false;
        }
        return true;
    }
} // namespace pawn::seats
