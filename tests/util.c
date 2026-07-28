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

#include "util.h"

void
assert_json(
    struct json_object *actual,
    struct json_object *expected,
    const char         *domain,
    const char         *file,
    int                 line,
    const char         *func
)
{
    if (json_object_equal(actual, expected))
        return;

    const char *actual_str =
        json_object_to_json_string_ext(actual, JSON_C_TO_STRING_PRETTY);
    const char *expected_str =
        json_object_to_json_string_ext(expected, JSON_C_TO_STRING_PRETTY);

    g_autofree char *message = g_strdup_printf(
        "JSON objects are not equal.\n"
        "Expected:\n%s\n"
        "Actual:\n%s\n",
        expected_str ? expected_str : "(null)",
        actual_str ? actual_str : "(null)"
    );

    g_assertion_message(domain, file, line, func, message);
}
