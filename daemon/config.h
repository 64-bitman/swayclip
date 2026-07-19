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

#include "common/sc/sc_array.h"
#include <stdbool.h>
#include <stdint.h>
#include <regex.h>

sc_array_def(regex_t, regex);

struct config_seat
{
    char *name;
    bool  regular;
    bool  primary;
};
sc_array_def(struct config_seat, config_seat);

struct config
{
    // Maximum number of entries to store in database, must be > 0
    int64_t max_entries;

    // Maximum size of the data from a mime type that will be saved. If bigger,
    // then the mime type will be ignored. In bytes
    int64_t max_size;

    // Enable persistent history
    bool persist;

    // Array of seats that the user has configured. If empty, then use all
    // seats.
    struct sc_array_config_seat configured_seats;

    // Array of regex_t of mime types that are allowed to be saved. If empty,
    // then assume all mime types.
    struct sc_array_regex allowed_mime_types;

    // Array of regex_t of mime types that will make the selection event be
    // completely ignored.
    struct sc_array_regex blocked_mime_types;

    // Array of regex_t of mime types that will create an entry, but not store
    // it in the database. This means it will still be persisted across
    // clipboard clears.
    struct sc_array_regex transient_mime_types;
};

// clang-format off
bool config_init(struct config *config, const char *file);
void config_uninit(struct config *config);
// clang-format on
