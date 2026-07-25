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

#include "xarray.h"
#include <json.h>

// Messages are in the format of <<payload size><payload>, where <payload size>
// is an unsigned 32 bit integer in native byte order. The message may have an
// associated file descriptor with it using SCM_RIGHTS. This fd will be mmapped
// to "aux_data" with the length of it being "aux_data_len".
struct ipc_message
{
    struct json_object *payload;
    void               *aux_data; // May be NULL
    size_t              aux_data_len;
};

struct ipc_write
{
    uint8_t *data;
    uint32_t size;
    uint32_t remaining;
    int      scm_fd; // Set to -1 if none
};

xarray_create(struct ipc_write, ipc_write, uint32_t, 32, 2);

struct ipc_ct
{
    int fd;

    uint32_t             pending_size;
    int                  scm_fd;
    struct json_tokener *tokener;
    uint8_t              buf[4096];

    struct xarray_ipc_write write_queue;
};

typedef void (*ipc_msg_callback)(struct ipc_message *msg, void *udata);

// clang-format off
bool ipc_ct_init(struct ipc_ct *ict, int fd);
void ipc_ct_uninit(struct ipc_ct *ict);
bool ipc_ct_read(struct ipc_ct *ict, bool need_scm, ipc_msg_callback callback, void *udata);
bool ipc_ct_write(struct ipc_ct *ict);
bool ipc_ct_has_pending_writes(struct ipc_ct *ict);
void ipc_ct_write_msg(struct ipc_ct *ict, struct json_object *msg, int scm_fd);
// clang-format on
