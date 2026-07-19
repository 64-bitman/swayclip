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
    } stmt;
};

// clang-format off
bool database_init(struct database *db, const char *dir, struct config *config);
void database_uninit(struct database *db);
bool database_save_setting(struct database *db, const char *key, int type, ...);
bool database_get_setting(struct database *db, const char *key, int type, ...);
// clang-format on
