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
 * Build a JSON object or modify an existing one using the given arguments.
 * Variadic argument are in the format of <key name>, <struct json_object>. Must
 * be terminated with NULL.
 *
 * If "add_flags" is -1, then use default flags.
 */
struct json_object *
build_json_object(struct json_object *obj, int add_flags, ...)
{
    if (obj == NULL)
        obj = json_object_new_object();

    if (obj == NULL)
        return NULL;

    if (add_flags == -1)
        add_flags =
            JSON_C_OBJECT_ADD_CONSTANT_KEY | JSON_C_OBJECT_ADD_KEY_IS_NEW;

    va_list ap;

    va_start(ap, add_flags);

    while (true)
    {
        const char *key = va_arg(ap, const char *);

        if (key == NULL)
            break;

        struct json_object *val = va_arg(ap, struct json_object *);

        if (json_object_object_add_ex(obj, key, val, add_flags) == -1)
            json_object_put(val);
    }

    va_end(ap);

    return obj;
}

/*
 * Same as build_json_object(), but values are pointers to store the value. Note
 * that null type is not supported. Returns true on success and false on
 * failure.
 */
bool
extract_json_object(struct json_object *obj, ...)
{
    bool    ret = false;
    va_list ap;

    va_start(ap, obj);

    while (true)
    {
        const char *key = va_arg(ap, const char *);

        if (key == NULL)
            break;

        int type = va_arg(ap, int);

        struct json_object *val = json_object_object_get(obj, key);

        if (val == NULL)
            goto exit;

        switch (type)
        {
        case 'b':
            if (!json_object_is_type(val, json_type_boolean))
                goto exit;
            *(va_arg(ap, bool *)) = json_object_get_boolean(val);
            break;
        case 'd':
            if (!json_object_is_type(val, json_type_double))
                goto exit;
            *(va_arg(ap, double *)) = json_object_get_double(val);
            break;
        case 'i':
            if (!json_object_is_type(val, json_type_int))
                goto exit;
            *(va_arg(ap, int64_t *)) = json_object_get_int64(val);
            break;
        case 'o':
            if (!json_object_is_type(val, json_type_object))
                goto exit;
            *(va_arg(ap, struct json_object **)) = val;
            break;
        case 'a':
            if (!json_object_is_type(val, json_type_array))
                goto exit;
            *(va_arg(ap, struct json_object **)) = val;
            break;
        case 's':
            if (!json_object_is_type(val, json_type_string))
                goto exit;
            *(va_arg(ap, const char **)) = json_object_get_string(val);
            break;
        case 'S':
            if (!json_object_is_type(val, json_type_string))
                goto exit;
            *(va_arg(ap, const char **)) = json_object_get_string(val);
            *(va_arg(ap, int *)) = json_object_get_string_len(val);
            break;
        default:
            log_abort("Unknown json type %c", type);
            break;
        }
    }

    ret = true;
exit:
    va_end(ap);

    return ret;
}

struct json_object *
build_json_array(struct json_object *arr, ...)
{
    if (arr == NULL)
        arr = json_object_new_array();

    if (arr == NULL)
        return NULL;

    va_list ap;

    va_start(ap, arr);

    while (true)
    {
        struct json_object *val = va_arg(ap, struct json_object *);

        if (val == NULL)
            break;

        json_object_array_add(arr, val);
    }

    va_end(ap);

    return arr;
}
