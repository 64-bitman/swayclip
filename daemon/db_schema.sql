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
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS Settings (
    Key             TEXT PRIMARY KEY,
    Value           NOT NULL
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Entries (
    Id              INTEGER PRIMARY KEY AUTOINCREMENT,
    Creation_time   INTEGER NOT NULL,
    Update_time     INTEGER NOT NULL,
    Pinned          BOOLEAN NOT NULL
);

CREATE TABLE IF NOT EXISTS Mime_types (
    Id              INTEGER NOT NULL,
    Mime_type       TEXT NOT NULL,
    Data_id         BLOB(32),
    PRIMARY KEY (Id, Mime_type),
    FOREIGN KEY (Id) REFERENCES Entries(Id) ON DELETE CASCADE,
    FOREIGN KEY (Data_id) REFERENCES Data(Data_id) ON DELETE RESTRICT
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS Data (
    Data_id         BLOB(32) PRIMARY KEY,
    Data            BLOB NOT NULL
) WITHOUT ROWID;

CREATE TRIGGER IF NOT EXISTS trim_entries
AFTER INSERT ON Entries BEGIN
DELETE FROM Entries WHERE Creation_time IN (
    SELECT Creation_time FROM Entries WHERE Pinned = 0
    ORDER BY Creation_time DESC LIMIT -1 OFFSET (
        SELECT Value FROM Settings WHERE Key = 'Max_entries'
    )
);
END;

CREATE TRIGGER IF NOT EXISTS del_data_row
AFTER DELETE ON main.Mime_types BEGIN
DELETE FROM Data WHERE Data_id = OLD.Data_id
AND NOT EXISTS (SELECT 1 FROM Mime_types WHERE
    Data_id = OLD.Data_id);
END;

CREATE TRIGGER IF NOT EXISTS del_data_row_on_update
AFTER UPDATE OF Data_id ON main.Mime_types BEGIN
DELETE FROM Data WHERE Data_id = OLD.Data_id
AND NOT EXISTS (SELECT 1 FROM Mime_types WHERE
    Data_id = OLD.Data_id);
END;
