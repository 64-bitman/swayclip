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

#include "common/ipc_ct.h"
#include <gio/gio.h>
#include <glib-object.h>
#include <gtk/gtk.h>
#include <json.h>

#define SWAYCLIP_TYPE_MESSAGE (swayclip_message_get_type())
typedef struct
{
    enum ipc_message_type type;
    struct json_object   *obj;
    GMappedFile          *aux_data; // NULL If none
} SwayclipMessage;

#define SWAYCLIP_TYPE_CONNECTION (swayclip_connection_get_type())
// clang-format off
G_DECLARE_FINAL_TYPE(SwayclipConnection, swayclip_connection, SWAYCLIP, CONNECTION, GObject)
// clang-format on

// clang-format off
SwayclipConnection *swayclip_connection_new(void);
void swayclip_connection_request(SwayclipConnection *self, struct json_object *obj, int scm_fd, int io_priority, GCancellable *cancellable, GAsyncReadyCallback callback, void *udata);
SwayclipMessage *swayclip_connection_request_finish(SwayclipConnection *self, GAsyncResult *result, GError **error);
SwayclipMessage *swayclip_message_copy(SwayclipMessage *msg);
void swayclip_message_free(SwayclipMessage *msg);
// clang-format on

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SwayclipMessage, swayclip_message_free);
