/*
 * swayclip
 * Copyright (C) 2026 Foxe Chen
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * To generate header file, use:
 * python db_schema_gen.py
 */

PRAGMA foreign_keys = ON;
PRAGMA recursive_triggers = NO;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS Settings (
    Key             TEXT PRIMARY KEY,
    Value           ANY NOT NULL
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Entries (
    -- Unique identifier for this specific entry, not guaranteed to be contiguous
    Id              INTEGER PRIMARY KEY AUTOINCREMENT,
    -- Used to order entries, may be updated (should always be greater than
        -- zero). Not guaranteed to be contiguous
    Sort_index      INTEGER NOT NULL UNIQUE DEFAULT 0,
    Creation_time   INTEGER NOT NULL, -- In milliseconds since unix epoch
    Update_time     INTEGER NOT NULL, -- Same thing
    Pinned          INTEGER NOT NULL CHECK (Pinned IN (0, 1)),
    -- Unique identifier for the *contents* of this entry. If NULL then ignore
    Hash            BLOB UNIQUE,
    Attributes      TEXT NOT NULL DEFAULT '{}' -- JSON data
) STRICT;

CREATE TABLE IF NOT EXISTS Mime_types (
    Id              INTEGER NOT NULL,
    Mime_type       TEXT NOT NULL,
    -- If NULL, then no data associated with entry.
    Data_id         BLOB CHECK (LENGTH(Data_id) = 32),
    PRIMARY KEY (Id, Mime_type),
    FOREIGN KEY (Id) REFERENCES Entries(Id) ON DELETE CASCADE,
    FOREIGN KEY (Data_id) REFERENCES Data(Data_id) ON DELETE RESTRICT
) STRICT, WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Data (
    Data_id         BLOB CHECK (LENGTH(Data_id) = 32) NOT NULL UNIQUE,
    Data            BLOB NOT NULL
) STRICT; -- rowid needed for incremental blob i/o

CREATE INDEX IF NOT EXISTS idx_mime_types_data_id ON Mime_types (Data_id);
CREATE INDEX IF NOT EXISTS idx_entries_pinned_creation ON Entries (Pinned, Id DESC);

-- Don't trim pinned entries
CREATE TRIGGER IF NOT EXISTS trim_entries
AFTER INSERT ON Entries
WHEN (SELECT Value FROM Settings WHERE Key = 'Max_entries') IS NOT NULL BEGIN
    DELETE FROM Entries WHERE Id IN (
        SELECT Id FROM Entries WHERE Pinned = 0
        ORDER BY Sort_index DESC LIMIT -1 OFFSET (
            SELECT CAST(Value AS INTEGER) FROM Settings WHERE Key = 'Max_entries'
        )
    );
END;

CREATE TRIGGER IF NOT EXISTS del_data_row
AFTER DELETE ON Mime_types
WHEN OLD.Data_id IS NOT NULL BEGIN
    DELETE FROM Data WHERE Data_id = OLD.Data_id
    AND NOT EXISTS (SELECT 1 FROM Mime_types WHERE Data_id = OLD.Data_id);
END;

CREATE TRIGGER IF NOT EXISTS del_data_row_on_update
AFTER UPDATE OF Data_id ON Mime_types
WHEN OLD.Data_id IS NOT NULL AND OLD.Data_id IS NOT NEW.Data_id BEGIN
    DELETE FROM Data WHERE Data_id = OLD.Data_id
    AND NOT EXISTS (SELECT 1 FROM Mime_types WHERE Data_id = OLD.Data_id);
END;

-- Automatically increment the sort index on each insertion
CREATE TRIGGER IF NOT EXISTS set_sort_index
AFTER INSERT ON Entries
WHEN NEW.Sort_index = 0
BEGIN
    UPDATE Entries
    SET Sort_index = (SELECT COALESCE(MAX(Sort_index), 0) + 1 FROM Entries WHERE Id != NEW.Id)
    WHERE Id = NEW.Id;
END;
