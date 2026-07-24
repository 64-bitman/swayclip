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

#define IPC_SUCCESS "success", 'b', true

// clang-format off
bool ipc_init(struct ipc *ipc, struct eventloop *loop, ipc_request_callback callback, void *udata);
void ipc_uninit(struct ipc *ipc);
bool ipc_client_start_array(struct ipc_client *client, int len);
void ipc_client_add_error(struct ipc_client *client, const char *desc);
void ipc_client_add_success(struct ipc_client *client);
// clang-format on
