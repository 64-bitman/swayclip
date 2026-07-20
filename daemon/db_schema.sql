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

-- Ensure Max_entries always exists so trim_entries never OFFSETs against NULL.
INSERT OR IGNORE INTO Settings (Key, Value) VALUES ('Max_entries', 1000);

CREATE TABLE IF NOT EXISTS Entries (
    Id              INTEGER PRIMARY KEY AUTOINCREMENT,
    Creation_time   INTEGER NOT NULL,
    Update_time     INTEGER NOT NULL,
    Pinned          INTEGER NOT NULL CHECK (Pinned IN (0, 1))
) STRICT;

CREATE TABLE IF NOT EXISTS Mime_types (
    Id              INTEGER NOT NULL,
    Mime_type       TEXT NOT NULL,
    Data_id         BLOB CHECK (LENGTH(Data_id) = 32), -- If NULL, then no data associated with entry.
    PRIMARY KEY (Id, Mime_type),
    FOREIGN KEY (Id) REFERENCES Entries(Id) ON DELETE CASCADE,
    FOREIGN KEY (Data_id) REFERENCES Data(Data_id) ON DELETE RESTRICT
) STRICT, WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_mime_types_data_id
ON Mime_types (Data_id);

CREATE TABLE IF NOT EXISTS Data (
    Data_id         BLOB CHECK (LENGTH(Data_id) = 32) NOT NULL UNIQUE,
    Data            BLOB NOT NULL
) STRICT; -- rowid needed for incremental blob i/o

CREATE INDEX IF NOT EXISTS idx_entries_pinned_creation
ON Entries (Pinned, Id DESC);

CREATE TRIGGER IF NOT EXISTS trim_entries
AFTER INSERT ON Entries
WHEN (SELECT Value FROM Settings WHERE Key = 'Max_entries') IS NOT NULL BEGIN
    DELETE FROM Entries WHERE Id IN (
        SELECT Id FROM Entries WHERE Pinned = 0
        ORDER BY Id DESC LIMIT -1 OFFSET (
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
