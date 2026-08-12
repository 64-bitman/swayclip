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

#include "util.h"
#include <json.h>
#include <stdbool.h>

#define JUTIL_FLAGS                                                            \
    JSON_C_OBJECT_ADD_CONSTANT_KEY | JSON_C_OBJECT_ADD_KEY_IS_NEW

#define JSON_VAL_NULL() json_object_new_null()
#define JSON_VAL_BOOL(v) json_object_new_boolean(v)
#define JSON_VAL_DOUBLE(v) json_object_new_double(v)
#define JSON_VAL_INT(v) json_object_new_int64(v)
#define JSON_VAL_OBJ(v) (v)
#define JSON_VAL_ARRAY(v) (v)
#define JSON_VAL_STR(v) json_object_new_string(v)
#define JSON_VAL_STRL(v, l) json_object_new_string_len(v, l)

#define JSON_FIELD_NULL(k) (k), JSON_VAL_NULL()
#define JSON_FIELD_BOOL(k, v) (k), JSON_VAL_BOOL(v)
#define JSON_FIELD_DOUBLE(k, v) (k), JSON_VAL_DOUBLE(v)
#define JSON_FIELD_INT(k, v) (k), JSON_VAL_INT(v)
#define JSON_FIELD_OBJ(k, v) (k), JSON_VAL_OBJ(v)
#define JSON_FIELD_ARRAY(k, v) (k), JSON_VAL_ARRAY(v)
#define JSON_FIELD_STR(k, v) (k), JSON_VAL_STR(v)
#define JSON_FIELD_STRL(k, v, l) (k), JSON_VAL_STRL(v, l)

#define JSON_EXTRACT_BOOL(k, v) (k), 'b', (v)
#define JSON_EXTRACT_DOUBLE(k, v) (k), 'd', (v)
#define JSON_EXTRACT_INT(k, v) (k), 'i', (v)
#define JSON_EXTRACT_OBJ(k, v) (k), 'o', (v)
#define JSON_EXTRACT_ARRAY(k, v) (k), 'a', (v)
#define JSON_EXTRACT_STR(k, v) (k), 's', (v)
#define JSON_EXTRACT_STRL(k, v, l) (k), 'S', (v), (l)

// clang-format off
struct json_object *build_json_object(struct json_object *obj, int add_flags, ...) SENTINEL;
bool extract_json_object(struct json_object *obj, ...) SENTINEL;
struct json_object *build_json_array(struct json_object *arr, ...) SENTINEL;
// clang-format on
