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
    } stmt;
};

// clang-format off
bool database_init(struct database *db, const char *dir, struct config *config);
void database_uninit(struct database *db);
bool database_save_setting(struct database *db, const char *key, int type, ...);
bool database_get_setting(struct database *db, const char *key, int type, ...);
bool database_do_transaction(struct database *db, enum database_transaction type);
int64_t database_new_entry(struct database *db);
bool database_new_mime_type(struct database *db, int64_t id, const char *mime_type, const uint8_t *data_id, uint8_t *data, size_t len);
// clang-format on
