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

#include "database.h"
#include "common/io.h"
#include "common/log.h"
#include "common/sha256/sha256.h"
#include "common/xdg.h"
#include "db_schema.h"
#include <errno.h>
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
        "INSERT INTO Entries (Creation_time, Update_time, Pinned) VALUES (?, "
        "?, FALSE) Returning Id;"
    ),
    STMT(
        new_mime_type,
        "INSERT OR IGNORE INTO Mime_types  (Id, Mime_type, Data_id) VALUES (?, "
        "?, ?);"
    ),
    STMT(new_data, "INSERT OR IGNORE INTO Data (Data_id, Data) VALUES (?, ?)")
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

bool
database_init(struct database *db, const char *dir, struct config *config)
{
    char *path;

    if (config->persist)
    {
        char *tofree = NULL;

        if (dir == NULL)
        {
            tofree = xdg_get_base_dir(XDG_DATA_HOME, "swayclip");
            dir = tofree;
        }
        if (dir == NULL)
            return false;
        if (mkdir(dir, 0755) == -1 && errno != EEXIST)
        {
            log_errerror("Error creating directory '%s'", dir);
            free(tofree);
            return false;
        }

        path = xstrdup_printf("%s/history.sqlite3", dir);
        free(tofree);
    }
    else
        path = strdup(":memory:");

    if (path == NULL)
        return false;

    int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    int ret = sqlite3_open_v2(path, &db->handle, flags, NULL);

    free(path);
    if (ret != SQLITE_OK)
    {
        log_error(
            "Error opening database at '%s': %s",
            dir,
            sqlite3_errmsg(db->handle)
        );

        if (db->handle != NULL)
            sqlite3_close(db->handle);
        return false;
    }

    // Set up database schema
    if (!database_execute_statement(db, db_schema))
    {
        sqlite3_close(db->handle);
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

    database_save_setting(
        db, "Max_entries", SQLITE_INTEGER, config->max_entries
    );

    return true;
}

void
database_uninit(struct database *db)
{
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
    case SQLITE_INTEGER:
        sqlite3_bind_int64(stmt, 2, va_arg(ap, int64_t));
        break;
    case SQLITE_BLOB:
    {
        const uint8_t *blob = va_arg(ap, const uint8_t *);
        int            sz = va_arg(ap, int);

        sqlite3_bind_blob(stmt, 2, blob, sz, SQLITE_STATIC);
        break;
    }
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
 * third is pointer to buffer length.
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
        log_error(
            "Error getting value of setting \"%s\" from database: %s",
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
    case SQLITE_INTEGER:
        *(va_arg(ap, int64_t *)) = sqlite3_column_int64(stmt, 0);
        break;
    case SQLITE_BLOB:
    {
        uint8_t *buf = va_arg(ap, uint8_t *);
        int      sz = va_arg(ap, int);
        int     *len = va_arg(ap, int *);

        const uint8_t *data = sqlite3_column_blob(stmt, 0);
        int            size = sqlite3_column_bytes(stmt, 0);

        memcpy(buf, data, MIN(size, sz));
        *len = size;

        break;
    }
    default:
        log_abort("Unsupported type %d for database setting", type);
    }

    va_end(ap);
    sqlite3_reset(stmt);

    return true;
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
        log_warn(
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
        log_warn(
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

    sqlite3_bind_blob(stmt, 1, data_id, SHA256_BLOCK_SIZE, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, data, len, SQLITE_STATIC);

    int ret = sqlite3_step(stmt);

    sqlite3_reset(stmt);
    if (ret != SQLITE_DONE)
    {
        log_warn(
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
        log_warn(
            "Error adding mime type \"%s\" to database: %s",
            mime_type,
            sqlite3_errmsg(db->handle)
        );
        return false;
    }

    return true;
}
