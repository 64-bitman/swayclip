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

#include "xarray.h"
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>

xarray_create(regex_t, regex, uint32_t, 4, 1.5);

struct config_seat
{
    char *name;
    bool  regular;
    bool  primary;
};
xarray_create(struct config_seat, config_seat, uint32_t, 4, 1.5);

enum config_dedup
{
    DEDUP_NONE,
    DEDUP_PREV,
    DEDUP_ALL
};

struct config
{
    // Maximum number of entries to store in database, must be > 0
    int64_t max_entries;

    // Maximum size of the data from a mime type that will be saved. If bigger,
    // then the mime type will be ignored. In bytes
    int64_t max_size;

    // Enable persistent history
    bool persist;

    // Default values for any seat
    bool regular;
    bool primary;

    bool set_on_startup;

    // CLI arguments take precendence
    char *logfile;
    char *db;

    // Setting for deduplication of entries.
    // "none" - don't deduplicate entries
    // "prev" - only check previous entry
    // "all" - Deduplicate all matching entries in history
    enum config_dedup dedup;

    // Array of seats that the user has configured. If empty then allow any
    // seat.
    struct xarray_config_seat configured_seats;

    // Array of regex_t of mime types that are allowed to be saved. If empty,
    // then assume all mime types.
    struct xarray_regex allowed_mime_types;

    // Array of regex_t of mime types that will make the selection event be
    // completely ignored.
    struct xarray_regex blocked_mime_types;
};

// clang-format off
bool config_init(struct config *config, const char *file);
void config_uninit(struct config *config);
// clang-format on
