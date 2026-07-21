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

#include "json_util.h"
#include "log.h"
#include <stdarg.h>
#include <stdbool.h>

/*
 * Build a JSON object using the given arguments. Variadic argument are in the
 * format of <key name>, <type>, <value>. Must be terminated with NULL
 * sentinel.
 *
 * Valid types are:
 * 'n': json_type_null,
 * 'b': json_type_boolean,
 * 'd': json_type_double,
 * 'i': json_type_int (int64_t)
 * 'o': json_type_object,
 * 's': json_type_string (no length arg),
 * 'S': json_type_string (with length arg)
 *
 * Returns object on success and NULL on failure
 */
struct json_object *
build_json_object(unsigned add_flags, ...)
{
    struct json_object *obj = json_object_new_object();

    if (obj == NULL)
        return NULL;

    va_list ap;

    va_start(ap, add_flags);

    while (true)
    {
        const char *key = va_arg(ap, const char *);

        if (key == NULL)
            break;

        int type = va_arg(ap, int);

        switch (type)
        {
        case 'n':
            json_object_object_add_ex(
                obj, key, json_object_new_null(), add_flags
            );
            break;
        case 'b':
            json_object_object_add_ex(
                obj, key, json_object_new_boolean(va_arg(ap, int)), add_flags
            );
            break;
        case 'd':
            json_object_object_add_ex(
                obj, key, json_object_new_double(va_arg(ap, double)), add_flags
            );
            break;
        case 'i':
            json_object_object_add_ex(
                obj, key, json_object_new_int64(va_arg(ap, int64_t)), add_flags
            );
            break;
        case 'o':
            json_object_object_add_ex(
                obj, key, va_arg(ap, struct json_object *), add_flags
            );
            break;
        case 's':
            json_object_object_add_ex(
                obj,
                key,
                json_object_new_string(va_arg(ap, const char *)),
                add_flags
            );
            break;
        case 'S':
        {
            const char *str = va_arg(ap, const char *);
            int         len = va_arg(ap, int);

            json_object_object_add_ex(
                obj, key, json_object_new_string_len(str, len), add_flags
            );
            break;
        }
        default:
            log_abort("Unknown json type %c", type);
            break;
        }
    }

    va_end(ap);

    return obj;
}
