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

#pragma once

#include "config.h"
#include <sqlite3.h>
#include <stdbool.h>

enum database_transaction
{
    DB_TRANSACTION_BEGIN,
    DB_TRANSACTION_IMMEDIATE,
    DB_TRANSACTION_COMMIT,
    DB_TRANSACTION_ROLLBACK,
};

struct database
{
    sqlite3 *handle;
    char    *path; // Path to sqlite database file

    struct config *config;

    struct
    {
        sqlite3_stmt *begin_transaction;
        sqlite3_stmt *begin_immediate;
        sqlite3_stmt *commit_transaction;
        sqlite3_stmt *rollback_transaction;

        sqlite3_stmt *save_setting;
        sqlite3_stmt *get_setting;

        sqlite3_stmt *new_entry;
        sqlite3_stmt *new_mime_type;
        sqlite3_stmt *new_data;

        sqlite3_stmt *get_mime_types;
        sqlite3_stmt *get_data_rowid;

        sqlite3_stmt *get_entries;

        sqlite3_stmt *get_history_size;
        sqlite3_stmt *id_exists;

        sqlite3_stmt *del_entry;
        sqlite3_stmt *update_entry;

        sqlite3_stmt *save_attribute;
        sqlite3_stmt *get_attribute;
    } stmt;
};

#define DB_SETTING_MAX_ENTRIES "Max_entries"
#define DB_SETTING_SELECTION_HASH "Selection_hash"
#define DB_SETTING_LAST_ENTRY "Last_entry"

#define DB_ATTRIBUTE_CONTENT_TYPE "$.content_type"
#define DB_ATTRIBUTE_CONTENT_MIME "$.content_mime"

// clang-format off
typedef void (*db_mime_type_callback)(const char *mime_type, void *udata);
typedef void (*db_entry_callback)(int64_t id, int64_t creation_time, int64_t update_time, bool pinned, void *udata);
// clang-format on

// clang-format off
bool database_init(struct database *db, const char *db_path, struct config *config);
void database_uninit(struct database *db);
bool database_save_setting(struct database *db, const char *key, int type, ...);
bool database_get_setting(struct database *db, const char *key, int type, ...);
bool database_do_transaction(struct database *db, enum database_transaction type);
int64_t database_new_entry(struct database *db);
bool database_new_mime_type(struct database *db, int64_t id, const char *mime_type, const uint8_t *data_id, uint8_t *data, size_t len);
bool database_get_mime_types(struct database *db, int64_t id, db_mime_type_callback callback, void *udata);
sqlite3_blob *database_get_data(struct database *db, int64_t id, const char *mime_type);
bool database_get_entries(struct database *db, int64_t start, int64_t n, db_entry_callback callback, void *udata);
int64_t database_get_history_size(struct database *db);
bool database_id_exists(struct database *db, int64_t id);
bool database_delete_entry(struct database *db, int64_t id);
int64_t database_update_entry(struct database *db, int64_t id, const bool *pinned);
bool database_save_attribute(struct database *db, int64_t id, const char *key, int type, ...);
bool database_get_attribute(struct database *db, int64_t id, const char *key, int type, ...);
// clang-format on
