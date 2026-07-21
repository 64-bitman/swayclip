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

#include <glib.h>

typedef enum
{
    SELECTION_REGULAR,
    SELECTION_PRIMARY
} SelectionType;

typedef struct Client Client;

// clang-format off
Client *client_new(const char *display);
void client_free(Client *client);
const char *client_get_seat(Client *client);
void client_copy(Client *client, SelectionType sel, ...);
GHashTable *client_paste(Client *client, SelectionType sel);
const char *client_paste_mime(Client *client, SelectionType sel, const char *mime_type, size_t *len);
// clang-format on

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Client, client_free)
