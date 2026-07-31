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

#define JSON_NULL(k, v) k, 'n', v
#define JSON_BOOL(k, v) k, 'b', v
#define JSON_DOUBLE(k, v) k, 'd', v
#define JSON_INT(k, v) k, 'i', v
#define JSON_OBJ(k, v) k, 'o', v
#define JSON_STR(k, v) k, 's', v
#define JSON_STRL(k, v) k, 'S', v

// clang-format off
struct json_object *build_json_object(struct json_object *obj, int add_flags, ...) SENTINEL;
bool extract_json_object(struct json_object *obj, ...) SENTINEL;
// clang-format on
