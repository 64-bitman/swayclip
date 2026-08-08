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

#include "swayclip-connection.h"
#include <gio/gio.h>
#include <glib-object.h>
#include <gtk/gtk.h>

typedef enum
{
    SWAYCLIP_CONTENT_UNKNOWN,
    SWAYCLIP_CONTENT_BINARY,
    SWAYCLIP_CONTENT_TEXT,
    SWAYCLIP_CONTENT_IMAGE,
} SwayclipContentType;

#define SWAYCLIP_TYPE_ENTRY (swayclip_entry_get_type())
G_DECLARE_FINAL_TYPE(SwayclipEntry, swayclip_entry, SWAYCLIP, ENTRY, GObject)

#define SWAYCLIP_TYPE_LIST (swayclip_list_get_type())
// clang-format off
G_DECLARE_FINAL_TYPE(SwayclipList, swayclip_list, SWAYCLIP, LIST, GObject)
// clang-format on

// clang-format off
SwayclipList *swayclip_list_new(SwayclipConnection *ct);
// clang-format on
