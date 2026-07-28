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

#include <gio/gio.h>
#include <glib.h>
#include <json.h>

#define UNUSED G_GNUC_UNUSED

#define ERROR _err

#define ASSERT_NOERROR(code)                                                   \
    do                                                                         \
    {                                                                          \
        GError *ERROR = NULL;                                                  \
        {                                                                      \
            code;                                                              \
        }                                                                      \
        g_assert_no_error(ERROR);                                              \
    } while (FALSE)

#define ASSERT_SQLITE(s) g_assert_cmpint(s, ==, SQLITE_OK)

#define ASSERT_JSON(actual, expected)                                          \
    assert_json(                                                         \
        (actual), (expected), G_LOG_DOMAIN, __FILE__, __LINE__, G_STRFUNC      \
    )

// clang-format off
void assert_json(struct json_object *actual, struct json_object *expected, const char *domain, const char *file, int line, const char *func);
// clang-format on
