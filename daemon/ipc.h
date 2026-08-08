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

#include "common/event.h"
#include "common/ipc_ct.h"
#include "xlist.h"
#include <json.h>

struct ipc_client;

// clang-format off
typedef void (*ipc_request_callback)(struct ipc_client *client, struct ipc_message *req, void *udata);
// clang-format on

xlist_declare(ipc_client);

struct ipc
{
    struct eventloop *loop;

    char *path;
    char *lock_path;

    int fd;
    int lock_fd;

    struct xlist_ipc_client connections;

    ipc_request_callback callback;
    void                *callback_udata;
};

// clang-format off
bool ipc_init(struct ipc *ipc, struct eventloop *loop, ipc_request_callback callback, void *udata);
void ipc_uninit(struct ipc *ipc);
bool ipc_client_start_array(struct ipc_client *client, int len);
void ipc_event_entry_add(struct ipc *ipc, int64_t entry_id);
void ipc_event_entry_delete(struct ipc *ipc, int64_t entry_id);
void ipc_event_entry_update(struct ipc *ipc, int64_t entry_id, const int64_t *update_time, const bool *pinned);
void ipc_event_entry_move(struct ipc *ipc, int64_t entry_id, int64_t old_pos, int64_t new_pos);
void ipc_event_entry_state(struct ipc *ipc, int64_t entry_id, bool state);
void ipc_event_sync(struct ipc *ipc);
void ipc_client_send(struct ipc_client *client, struct json_object *msg, int scm_fd);
void ipc_client_send_error(struct ipc_client *client, const char *desc_fmt, ...);
void ipc_client_send_success(struct ipc_client *client);
void ipc_client_send_success_fd(struct ipc_client *client, int scm_fd);
void ipc_client_set_events(struct ipc_client *client, uint events);
// clang-format on
