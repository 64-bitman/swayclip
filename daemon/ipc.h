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
#include "common/sc/sc_list.h"
#include <json.h>

typedef void (*ipc_request_callback)(struct json_object *req, void *udata);

struct ipc
{
    struct eventloop *loop;

    char *path;
    char *lock_path;

    int fd;
    int lock_fd;

    struct sc_list connections;

    ipc_request_callback callback;
    void                *udata;
};

// clang-format off
bool ipc_init(struct ipc *ipc, struct eventloop *loop, ipc_request_callback callback, void *udata);
void ipc_uninit(struct ipc *ipc);
// clang-format on
