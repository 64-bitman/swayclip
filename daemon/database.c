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

#define USER_VERSION 2

#include "database.h"
#include "common/io.h"
#include "common/log.h"
#include "common/sha256/sha256.h"
#include "common/xdg.h"
#include "db_schema.h"
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <sys/stat.h>

struct stmt_def
{
    size_t      offset;
    const char *statement;
};
#define STMT(f, s)                                                             \
    (struct stmt_def) { offsetof(struct database, stmt.f), s }

static const struct stmt_def stmt_defs[] = {
    STMT(begin_transaction, "BEGIN TRANSACTION;"),
    STMT(begin_immediate, "BEGIN IMMEDIATE TRANSACTION;"),
    STMT(commit_transaction, "COMMIT;"),
    STMT(rollback_transaction, "ROLLBACK TRANSACTION;"),

    STMT(
        save_setting,
        "INSERT OR REPLACE INTO Settings (Key, Value) VALUES (?, ?);"
    ),
    STMT(get_setting, "SELECT Value FROM Settings WHERE Key = ?;"),

    STMT(
        new_entry,
        "INSERT INTO Entries (Creation_time, Update_time, Pinned) "
        "VALUES (?, ?, FALSE) Returning Id;"
    ),
    STMT(
        new_mime_type,
        "INSERT OR IGNORE INTO Mime_types  (Id, Mime_type, Data_id) VALUES (?, "
        "?, ?);"
    ),
    STMT(new_data, "INSERT OR IGNORE INTO Data (Data_id, Data) VALUES (?, ?)"),
    STMT(get_mime_types, "SELECT Mime_type FROM Mime_types WHERE Id = ?;"),
    STMT(
        get_data_rowid,
        "SELECT Data.rowid FROM Mime_types LEFT JOIN Data ON Data.Data_id = "
        "Mime_types.Data_id WHERE Mime_types.Id = ? AND Mime_types.Mime_type = "
        "?;"
    ),
    STMT(get_history_size, "SELECT COUNT(1) FROM Entries;"),
    STMT(
        get_entries,
        "SELECT Id, Creation_time, Update_time, Pinned FROM Entries "
        "ORDER BY Sort_index DESC LIMIT ? OFFSET ?;"
    ),
    STMT(id_exists, "SELECT 1 FROM Entries WHERE Id = ?;"),
    STMT(del_entry, "DELETE FROM Entries WHERE Id = ?;"),
    STMT(
        update_entry,
        "UPDATE Entries SET Update_time = ?, Pinned = COALESCE(?, Pinned) "
        "WHERE Id = ?;"
    ),
    STMT(
        save_attribute,
        "UPDATE Entries SET Attributes = json_set(COALESCE(Attributes, '{}'), "
        "?, ?) WHERE Id = ?;"
    ),
    STMT(
        get_attribute,
        "SELECT json_extract(Attributes, ?) FROM Entries WHERE Id = ?;"
    ),
    STMT(
        // Either Id or Hash can be provided, but not both.
        save_entry_hash,
        "UPDATE Entries SET Hash = ? WHERE Id = ? OR Hash = ?;"
    ),
    STMT(
        // Return the position of the matching entry
        entry_hash_pos,
        "SELECT Id, (SELECT COUNT(1) FROM Entries AS e2 WHERE e2.Sort_index > "
        "e.Sort_index) AS Position FROM Entries AS e WHERE e.Hash IS NOT "
        "NULL AND e.Hash = ?;"
    ),
    STMT(
        update_sort_index,
        "UPDATE Entries SET Sort_index = (SELECT COALESCE(MAX(Sort_index), 0) "
        "+ 1 FROM Entries) WHERE Id = ?;"
    ),
    STMT(get_pinned, "SELECT Pinned FROM Entries WHERE Id = ?;"),
    STMT(
        trim,
        "DELETE FROM Entries WHERE Id IN (SELECT Id FROM Entries WHERE Pinned "
        "= 0 ORDER BY Id DESC LIMIT -1 OFFSET ?) RETURNING Id;"
    )
};

static bool
database_execute_statement(struct database *db, const char *statement)
{
    char *err_msg = NULL;
    int   ret = sqlite3_exec(db->handle, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
    {
        log_error("Error executing statement '%s': %s", statement, err_msg);
        sqlite3_free(err_msg);
        return false;
    }
    return true;
}

static void
migrate_database(struct database *db, int old_ver)
{
    // Probably should put this in a for loop to incremently modify database in
    // case user version is bumped up again.
    if (old_ver == 1)
    {
        // Delete trim trigger, we now do that manually
        (void)database_execute_statement(
            db, "DROP TRIGGER IF EXISTS trim_entries;"
        );
    }
}

bool
database_init(struct database *db, const char *db_path, struct config *config)
{
    // TODO: handle user_version pragma
    char *path;

    if (config->persist)
    {
        if (db_path == NULL)
        {
            char *dir = xdg_get_base_dir(XDG_DATA_HOME, "swayclip");
            if (dir == NULL)
                return false;

            if (mkdir(dir, 0755) == -1 && errno != EEXIST)
            {
                log_errerror("Error creating directory '%s'", dir);
                free(dir);
                return false;
            }

            path = xstrdup_printf("%s/history.sqlite3", dir);
            free(dir);
        }
        else
            path = strdup(db_path);
    }
    else
        path = strdup(":memory:");

    if (path == NULL)
        return false;

    db->path = path;
    db->config = config;

    int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    int ret = sqlite3_open_v2(path, &db->handle, flags, NULL);

    if (ret != SQLITE_OK)
    {
        log_error(
            "Error opening database at '%s': %s",
            path,
            db->handle == NULL ? "" : sqlite3_errmsg(db->handle)
        );

        if (db->handle != NULL)
            sqlite3_close(db->handle);
        free(path);
        return false;
    }

    // Check user version
    {
        sqlite3_stmt *stmt;

        ret = sqlite3_prepare_v2(
            db->handle, "PRAGMA user_version;", -1, &stmt, NULL
        );
        if (ret != SQLITE_OK)
        {
            sqlite3_finalize(stmt);
            sqlite3_close(db->handle);
            free(path);
            return false;
        }

        ret = sqlite3_step(stmt);
        int user_ver = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        if (user_ver != USER_VERSION)
            migrate_database(db, user_ver);
    }

    // Set up database schema
    if (!database_execute_statement(db, db_schema))
    {
        sqlite3_close(db->handle);
        free(path);
        return false;
    }

    for (int i = 0; i < N_ELEMENTS(stmt_defs); i++)
    {
        const struct stmt_def *def = stmt_defs + i;
        sqlite3_stmt         **slot =
            (sqlite3_stmt **)(void *)((char *)db + def->offset);

        ret = sqlite3_prepare_v2(db->handle, def->statement, -1, slot, NULL);

        if (ret != SQLITE_OK)
        {
            log_error(
                "Error preparing database statement '%s': %s",
                def->statement,
                sqlite3_errmsg(db->handle)
            );
            database_uninit(db);
            return false;
        }
    }

    // Do an initial trim in case "max_entries" changed. Do not need to worry
    // about emitting "entry_delete" IPC event, because database is initialized
    // before IPC server.
    (void)database_trim(db, NULL, NULL);

    log_debug("Initialized databasae at \"%s\"", db->path);

    return true;
}

void
database_uninit(struct database *db)
{
    free(db->path);
    if (!database_execute_statement(db, "PRAGMA optimize;"))
        log_warn("Error optimizing database");

    for (int i = 0; i < N_ELEMENTS(stmt_defs); i++)
    {
        const struct stmt_def *def = stmt_defs + i;
        sqlite3_stmt         **slot =
            (sqlite3_stmt **)(void *)((char *)db + def->offset);

        if (sqlite3_stmt_busy(*slot))
            log_warn("Database still has statements active");
        sqlite3_finalize(*slot);
    }

    sqlite3_close(db->handle);
}

/*
 * Save setting entry in database, depending on "type".
 * "
 */
bool
database_save_setting(struct database *db, const char *key, int type, ...)
{
    sqlite3_stmt *stmt = db->stmt.save_setting;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    va_list ap;

    va_start(ap, type);
    switch (type)
    {
    case 'i':
        sqlite3_bind_int64(stmt, 2, va_arg(ap, int64_t));
        break;
    default:
        log_abort("Unsupported type %d for database setting", type);
    }
    va_end(ap);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error saving setting \"%s\" into database: %s",
            key,
            sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}

/*
 * Get value of setting from database. Variadic args is pointer to storage
 * location. For blobs, first arg is buffer, second arg is max buffer size,
 * third is pointer to buffer length (may be NULL).
 */
bool
database_get_setting(struct database *db, const char *key, int type, ...)
{
    sqlite3_stmt *stmt = db->stmt.get_setting;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        if (ret != SQLITE_DONE)
            log_error(
                "Error getting value of setting \"%s\" from database: %s",
                key,
                sqlite3_errmsg(db->handle)
            );
        sqlite3_reset(stmt);
        return false;
    }

    va_list ap;
    bool    res = true;

    va_start(ap, type);
    switch (type)
    {
    case 'i':
        *(va_arg(ap, int64_t *)) = sqlite3_column_int64(stmt, 0);
        break;
    default:
        log_abort("Unsupported type %d for database setting", type);
    }

    va_end(ap);
    sqlite3_reset(stmt);

    return res;
}

/*
 * Start a transaction for the database. Returns true on success and false on
 * failure.
 */
bool
database_do_transaction(struct database *db, enum database_transaction type)
{
    sqlite3_stmt *stmt;

    switch (type)
    {
    case DB_TRANSACTION_BEGIN:
        stmt = db->stmt.begin_transaction;
        break;
    case DB_TRANSACTION_IMMEDIATE:
        stmt = db->stmt.begin_immediate;
        break;
    case DB_TRANSACTION_COMMIT:
        stmt = db->stmt.commit_transaction;
        break;
    case DB_TRANSACTION_ROLLBACK:
        stmt = db->stmt.rollback_transaction;
        break;
    default:
        log_abort("Unknown transaction %d", type);
        break;
    }

    assert(!sqlite3_stmt_busy(stmt));

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error starting database transaction %d: %s",
            type,
            sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}

/*
 * Add new entry to database and return ID. Return -1 on failure.
 */
int64_t
database_new_entry(struct database *db)
{
    sqlite3_stmt *stmt = db->stmt.new_entry;

    assert(!sqlite3_stmt_busy(stmt));

    int64_t t = get_time_ns(CLOCK_REALTIME) / 1000000; // ms

    sqlite3_bind_int64(stmt, 1, t);
    sqlite3_bind_int64(stmt, 2, t);

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        log_error(
            "Error serializing entry into database: %s",
            sqlite3_errmsg(db->handle)
        );
        sqlite3_reset(stmt);
        return -1;
    }

    int64_t id = sqlite3_column_int64(stmt, 0);

    sqlite3_reset(stmt);

    return id;
}

static bool
database_new_data(
    struct database *db, const uint8_t *data_id, uint8_t *data, size_t len
)
{
    sqlite3_stmt *stmt = db->stmt.new_data;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_blob(stmt, 1, data_id, SHA256_BLOCK_SIZE, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, data, len, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error adding data into database: %s", sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}

/*
 * Add "data" into the database for "mime_type" associated with entry "id".
 * Returns true on success and false on failure.
 */
bool
database_new_mime_type(
    struct database *db,
    int64_t          id,
    const char      *mime_type,
    const uint8_t   *data_id,
    uint8_t         *data,
    size_t           len
)
{
    // If "data" is NULL, then "Data_id" in "Mime_types" table will just be
    // NULL.
    if (data != NULL && !database_new_data(db, data_id, data, len))
        return false;

    sqlite3_stmt *stmt = db->stmt.new_mime_type;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);
    if (data == NULL)
        sqlite3_bind_null(stmt, 3);
    else
        sqlite3_bind_blob(stmt, 3, data_id, SHA256_BLOCK_SIZE, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error adding mime type \"%s\" to database: %s",
            mime_type,
            sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}

bool
database_get_mime_types(
    struct database *db, int64_t id, db_mime_type_callback callback, void *udata
)
{
    sqlite3_stmt *stmt = db->stmt.get_mime_types;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, id);

    int ret;

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *mime_type = (char *)sqlite3_column_text(stmt, 0);

        callback(mime_type, udata);
    }

    sqlite3_reset(stmt);

    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error getting mime types from database: %s",
            sqlite3_errmsg(db->handle)
        );
        return false;
    }
    return true;
}

/*
 * Return the blob object for the mime type. Returns NULL on failure.
 */
sqlite3_blob *
database_get_data(struct database *db, int64_t id, const char *mime_type)
{
    sqlite3_stmt *stmt = db->stmt.get_data_rowid;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        if (ret != SQLITE_DONE)
            log_error(
                "Error getting mime type data from database: %s",
                sqlite3_errmsg(db->handle)
            );
        sqlite3_reset(stmt);
        return NULL;
    }

    if (sqlite3_column_type(stmt, 0) == SQLITE_NULL)
    {
        log_debug("No rowid for %" PRId64 " %s in Data table", id, mime_type);
        sqlite3_reset(stmt);
        return NULL;
    }

    int64_t rowid = sqlite3_column_int64(stmt, 0);
    sqlite3_reset(stmt);

    sqlite3_blob *blob = NULL;

    ret =
        sqlite3_blob_open(db->handle, "main", "Data", "Data", rowid, 0, &blob);

    if (ret != SQLITE_OK)
    {
        log_error("Error opening blob: %s", sqlite3_errmsg(db->handle));
        return NULL;
    }

    assert(blob != NULL);
    return blob;
}

bool
database_get_entries(
    struct database  *db,
    int64_t           start,
    int64_t           n,
    db_entry_callback callback,
    void             *udata
)
{
    sqlite3_stmt *stmt = db->stmt.get_entries;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, n);
    sqlite3_bind_int64(stmt, 2, start);

    int ret;

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        int64_t id = sqlite3_column_int64(stmt, 0);
        int64_t creation_time = sqlite3_column_int64(stmt, 1);
        int64_t update_time = sqlite3_column_int64(stmt, 2);
        bool    pinned = sqlite3_column_int(stmt, 3);

        callback(id, creation_time, update_time, pinned, udata);
    }

    sqlite3_reset(stmt);

    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error getting entries from database: %s",
            sqlite3_errmsg(db->handle)
        );
        return false;
    }
    return true;
}

/*
 * Return number of entries in history, or -1 on failure.
 */
int64_t
database_get_history_size(struct database *db)
{
    sqlite3_stmt *stmt = db->stmt.get_history_size;

    assert(!sqlite3_stmt_busy(stmt));

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        log_error(
            "Error getting number of entries: %s", sqlite3_errmsg(db->handle)
        );
        sqlite3_reset(stmt);
        return -1;
    }

    int64_t n = sqlite3_column_int64(stmt, 0);
    sqlite3_reset(stmt);
    return n;
}

bool
database_id_exists(struct database *db, int64_t id)
{
    sqlite3_stmt *stmt = db->stmt.id_exists;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, id);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret == SQLITE_ROW)
        return true;
    else if (ret == SQLITE_DONE)
        return false;

    log_error("Error checking if id exists: %s", sqlite3_errmsg(db->handle));
    return false;
}

bool
database_delete_entry(struct database *db, int64_t id)
{
    sqlite3_stmt *stmt = db->stmt.del_entry;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_int64(stmt, 1, id);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret == SQLITE_DONE)
        return true;

    log_error("Error deleting entry: %s", sqlite3_errmsg(db->handle));
    return false;
}

/*
 * If "pinned" is NULL, then it will be left unchanged. Return new update time
 * in milliseconds or -1 on error.
 */
int64_t
database_update_entry(struct database *db, int64_t id, const bool *pinned)
{
    sqlite3_stmt *stmt = db->stmt.update_entry;

    assert(!sqlite3_stmt_busy(stmt));

    int64_t t = get_time_ns(CLOCK_REALTIME) / 1000000; // ms

    sqlite3_bind_int64(stmt, 1, t);

    if (pinned != NULL)
        sqlite3_bind_int(stmt, 2, *pinned);
    else
        sqlite3_bind_null(stmt, 2);

    sqlite3_bind_int64(stmt, 3, id);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error("Error updating entry: %s", sqlite3_errmsg(db->handle));
        return -1;
    }

    return t;
}

/*
 * Save an attribute for the given entry, depending on "type". Note that "key"
 * must be in JSON path syntax. Returns true on success and false on failuure.
 */
bool
database_save_attribute(
    struct database *db, int64_t id, const char *key, int type, ...
)
{
    sqlite3_stmt *stmt = db->stmt.save_attribute;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, id);

    va_list ap;

    va_start(ap, type);
    switch (type)
    {
    case 's':
        sqlite3_bind_text(stmt, 2, va_arg(ap, const char *), -1, SQLITE_STATIC);
        break;
    default:
        log_abort("Unsupported type %d for entry attribute", type);
    }
    va_end(ap);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error saving attribute \"%s\": %s", key, sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}

/*
 * Similar to database_get_setting().
 */
bool
database_get_attribute(
    struct database *db, int64_t id, const char *key, int type, ...
)
{
    sqlite3_stmt *stmt = db->stmt.get_attribute;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, id);

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        if (ret != SQLITE_DONE)
            log_error(
                "Error getting attribute \"%s\": %s",
                key,
                sqlite3_errmsg(db->handle)
            );
        sqlite3_reset(stmt);
        return false;
    }

    va_list ap;

    va_start(ap, type);
    switch (type)
    {
    case 's':
    {
        char *buf = va_arg(ap, char *);
        int   sz = va_arg(ap, int);

        const char *text = (char *)sqlite3_column_text(stmt, 0);

        snprintf(buf, sz, "%s", text);
        break;
    }
    default:
        log_abort("Unsupported type %d for entry attribute", type);
    }

    va_end(ap);
    sqlite3_reset(stmt);

    return true;
}

/*
 * Set the entry hash as a blob. If "id" is -1, then NULL the Hash column of
 * every entry with matching hash "hash".
 */
bool
database_set_entry_hash(
    struct database *db, int64_t id, uint8_t hash[SHA256_BLOCK_SIZE]
)
{
    sqlite3_stmt *stmt = db->stmt.save_entry_hash;

    assert(!sqlite3_stmt_busy(stmt));

    if (id == -1)
    {
        sqlite3_bind_null(stmt, 1);
        sqlite3_bind_null(stmt, 2);
        sqlite3_bind_blob(stmt, 3, hash, SHA256_BLOCK_SIZE, SQLITE_STATIC);
    }
    else
    {
        sqlite3_bind_blob(stmt, 1, hash, SHA256_BLOCK_SIZE, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, id);
        sqlite3_bind_null(stmt, 3);
    }

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_error("Error saving entry hash: %s", sqlite3_errmsg(db->handle));
        return false;
    }

    return true;
}

/*
 * Return position (and optionally id) of entry with hash "hash", and -1 if no
 * entry with the hash or if an error occured.
 */
int64_t
database_entry_hash_pos(
    struct database *db, uint8_t hash[SHA256_BLOCK_SIZE], int64_t *id
)
{
    sqlite3_stmt *stmt = db->stmt.entry_hash_pos;

    assert(!sqlite3_stmt_busy(stmt));

    sqlite3_bind_blob(stmt, 1, hash, SHA256_BLOCK_SIZE, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    if (ret != SQLITE_ROW)
    {
        if (ret != SQLITE_DONE)
            log_error(
                "Error getting position of entry hash: %s",
                sqlite3_errmsg(db->handle)
            );
        sqlite3_reset(stmt);
        return -1;
    }

    int64_t idval = sqlite3_column_int64(stmt, 0);
    int64_t pos = sqlite3_column_int64(stmt, 1);

    if (id != NULL)
        *id = idval;

    sqlite3_reset(stmt);
    return pos;
}

/*
 * Update the sort index of the given entry so that it is at the front of the
 * clipboard history.
 */
bool
database_update_sort_index(struct database *db, int64_t id)
{
    sqlite3_stmt *stmt = db->stmt.update_sort_index;

    assert(!sqlite3_stmt_busy(stmt));
    sqlite3_bind_int64(stmt, 1, id);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret == SQLITE_DONE)
        return true;

    log_error("Error updating sort index: %s", sqlite3_errmsg(db->handle));
    return false;
}

bool
database_entry_is_pinned(struct database *db, int64_t id)
{
    sqlite3_stmt *stmt = db->stmt.get_pinned;

    assert(!sqlite3_stmt_busy(stmt));
    sqlite3_bind_int64(stmt, 1, id);

    int ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        bool pinned = sqlite3_column_int(stmt, 0);

        sqlite3_reset(stmt);
        return pinned;
    }

    sqlite3_reset(stmt);
    log_error(
        "Error getting pinned status of entry: %s", sqlite3_errmsg(db->handle)
    );
    return false;
}

/*
 * Trim excess entries in the database according to "config->max_entries", and
 * call "callback" for each entry deleted/trimmed. Returns true on success and
 * false on failure.
 */
bool
database_trim(struct database *db, db_trim_callback callback, void *udata)
{
    sqlite3_stmt *stmt = db->stmt.trim;

    assert(!sqlite3_stmt_busy(stmt));
    sqlite3_bind_int64(stmt, 1, db->config->max_entries);

    int ret;

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        int64_t id = sqlite3_column_int64(stmt, 0);

        if (callback != NULL)
            callback(id, udata);
        log_debug("Trimming entry %" PRId64 " from database", id);
    }

    sqlite3_reset(stmt);

    if (ret != SQLITE_DONE)
    {
        log_error(
            "Error trimming entries from database: %s",
            sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}
