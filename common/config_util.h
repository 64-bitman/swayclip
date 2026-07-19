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

#include "tomlc17/tomlc17.h"

// clang-format off
// Should return false on fatal error
typedef bool (*config_option_callback)(const char *key, toml_datum_t dat, void *store);
// clang-format on

// If key does not exist, then nothing is done.
struct config_option
{
    const char *key;
    toml_type_t type;
    void       *store;

    config_option_callback callback;
};

#define CONFIG_OPT(k, t, s, c)                                                 \
    (struct config_option) { .key = k, .type = t, .store = s, .callback = c }
#define CONFIG_STRING(k, s) CONFIG_OPT(k, TOML_STRING, s, config_extract_string)
#define CONFIG_INT64(k, s) CONFIG_OPT(k, TOML_INT64, s, config_extract_int64)
#define CONFIG_INT64_POS(k, s)                                                 \
    CONFIG_OPT(k, TOML_INT64, s, config_extract_int64_pos)
#define CONFIG_BOOLEAN(k, s)                                                   \
    CONFIG_OPT(k, TOML_BOOLEAN, s, config_extract_boolean)
#define CONFIG_TABLE(k, s, c) CONFIG_OPT(k, TOML_TABLE, s, c)
#define CONFIG_ARRAY(k, s, c) CONFIG_OPT(k, TOML_ARRAY, s, c)

// clang-format off
bool config_parse(const char *file, toml_result_t *result);
bool config_extract(toml_datum_t table, const struct config_option *opts, int n_opts);
bool config_extract_string(const char *ey, toml_datum_t dat, void *store);
bool config_extract_int64(const char *key, toml_datum_t dat, void *store);
bool config_extract_int64_pos(const char *key, toml_datum_t dat, void *store);
bool config_extract_boolean(const char *key, toml_datum_t dat, void *store);
// clang-format on
