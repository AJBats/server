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

#pragma once

#include "common/cbasetypes.h"
#include "common/database.h"
#include "common/logging.h"
#include "common/types/maybe.h"

#include <string>
#include <utility>

// Character creation shared with the census (tools/world/census.py), which mints the world's
// cardians as character rows while the servers run (RESEARCH §18.4). Three writers create
// characters -- the lobby, the census and the map's !pawncreate -- and the lobby's own creation is
// two steps minutes apart: the name is checked when the player types it, and the character is
// saved once he has chosen a look, without a second check. Two rules keep the writers apart:
//
//   the name hold   a name the lobby accepts is held for that account in cardian_name_holds
//                   until the character is saved, or for kHoldHours: the census never mints a
//                   held name, and another account is refused it
//   one at a time   creating a character takes one database lock (lockName), so no two writers
//                   take the same next character id, nor a name the other has just written
//
// The census takes the same lock under the same name and honours the same holds (census.py,
// CREATE_LOCK and NAME_HOLD_HOURS).
namespace cardian::lobby
{
    constexpr int32 kHoldHours   = 1;
    constexpr int32 kLockSeconds = 10;

    // Per database: every world folder's servers share one MariaDB, and a lock name is server-wide
    inline auto lockName() -> std::string
    {
        return "cardian_create:" + db::getDatabase().getSchema();
    }

    // The table is Cardian's, made on first use like cardian_clock, so no database needs a step to
    // have it (modules/cardian/sql/cardian_name_holds.sql documents it)
    inline void ensureTable()
    {
        static bool made = false;
        if (!made)
        {
            made = db::preparedStmt("CREATE TABLE IF NOT EXISTS cardian_name_holds ("
                                    "name VARCHAR(15) NOT NULL, accid INT(10) UNSIGNED NOT NULL, held_at DATETIME NOT NULL, "
                                    "PRIMARY KEY (name), KEY accid (accid)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4") != nullptr;
        }
    }

    // One character creation at a time with the other writers, for the guard's life. Given the name
    // being created, its hold ends with the guard. Waits up to kLockSeconds for a writer mid-creation
    // (a census claim holds the lock for milliseconds), then creates without it and says so
    class CreateTurn
    {
    public:
        explicit CreateTurn(std::string name = {})
        : name_(std::move(name))
        {
            ensureTable();
            const auto rset = db::preparedStmt("SELECT GET_LOCK(?, ?) AS got", lockName(), kLockSeconds);
            held_           = rset && rset->next() && rset->get<int32>("got") == 1;
            if (!held_)
            {
                ShowWarningFmt("cardian: character creation went ahead without the creation lock ({})", lockName());
            }
        }

        ~CreateTurn()
        {
            if (!name_.empty())
            {
                db::preparedStmt("DELETE FROM cardian_name_holds WHERE name = ?", name_);
            }
            if (held_)
            {
                db::preparedStmt("SELECT RELEASE_LOCK(?)", lockName());
            }
        }

        CreateTurn(const CreateTurn&)            = delete;
        CreateTurn& operator=(const CreateTurn&) = delete;

        // Whether no character carries the name yet: one whose hold lapsed may have been created by
        // another account meanwhile
        auto nameFree() const -> bool
        {
            const auto rset = db::preparedStmt("SELECT 1 FROM chars WHERE charname = ? LIMIT 1", name_);
            if (rset && rset->rowsCount() == 0)
            {
                return true;
            }
            ShowWarningFmt("cardian: {} was taken while its hold had lapsed; the creation is refused", name_);
            return false;
        }

    private:
        std::string name_;
        bool        held_ = false;
    };

    // Whether another account holds the name (accid 0: any account)
    inline auto heldByAnother(const std::string& name, const uint32 accid) -> bool
    {
        ensureTable();
        const auto rset = db::preparedStmt("SELECT accid FROM cardian_name_holds WHERE name = ? AND held_at >= NOW() - INTERVAL ? HOUR",
                                           name, kHoldHours);
        return rset && rset->next() && rset->get<uint32>("accid") != accid;
    }

    // The name a player typed at character creation, held for his account until the character is
    // saved: nullopt when it is held, else why not (worded as validateCharacterName words a refusal).
    // An account holds one name at a time
    inline auto holdName(const std::string& name, const uint32 accid) -> Maybe<std::string>
    {
        const CreateTurn turn;
        db::preparedStmt("DELETE FROM cardian_name_holds WHERE held_at < NOW() - INTERVAL ? HOUR", kHoldHours);
        if (const auto rset = db::preparedStmt("SELECT 1 FROM chars WHERE charname = ? LIMIT 1", name); !rset || rset->rowsCount() != 0)
        {
            return "Name already in use.";
        }
        if (heldByAnother(name, accid))
        {
            return "Name held for another account's new character.";
        }
        db::preparedStmt("DELETE FROM cardian_name_holds WHERE accid = ?", accid);
        db::preparedStmt("INSERT INTO cardian_name_holds (name, accid, held_at) VALUES (?, ?, NOW())", name, accid);
        return std::nullopt;
    }

    // The lobby's verdict on a typed name, and the hold when it passes
    inline auto thenHold(Maybe<std::string> refused, const std::string& name, const uint32 accid) -> Maybe<std::string>
    {
        return refused.has_value() ? std::move(refused) : holdName(name, accid);
    }
} // namespace cardian::lobby
