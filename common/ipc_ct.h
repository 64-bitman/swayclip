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

#include "sc/sc_array.h"
#include <json.h>

enum ipc_message_type
{
    IPC_MESSAGE_JSON, // Payload is JSON string
    IPC_MESSAGE_BLOB  // Payload is a binary blob
};

union ipc_payload
{
    struct json_object *json;
    struct
    {
        uint8_t *data;
        uint32_t size;
    } blob;
};

// Messages are in the format of <type><payload size><payload>, where <type> is
// a unsigned 8 bit integer and <payload size> are unsigned 32 bit integers
// in host order.
struct ipc_message
{
    enum ipc_message_type type;
    union ipc_payload     payload;
};

struct ipc_write
{
    uint8_t *data;
    uint32_t size;
    uint32_t remaining;
};

sc_array_def(struct ipc_write, ipc_write);

struct ipc_ct
{
    int fd;

    bool     got_header;
    uint32_t remaining;
    int      scm_fd; // -1 if not set

    struct json_tokener *tokener;

    uint8_t buf[4096];

    struct sc_array_ipc_write write_queue;
};

typedef void (*ipc_msg_callback)(struct ipc_message *msg, void *udata);

// clang-format off
bool ipc_ct_init(struct ipc_ct *ict, int fd);
void ipc_ct_uninit(struct ipc_ct *ict);
bool ipc_ct_read(struct ipc_ct *ict, ipc_msg_callback callback, void *udata);
bool ipc_ct_write(struct ipc_ct *ict);
bool ipc_ct_has_pending_writes(struct ipc_ct *ict);
void ipc_ct_write_msg(struct ipc_ct *ict, enum ipc_message_type type, union ipc_payload payload);
// clang-format on
