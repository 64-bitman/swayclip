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

#include "sc/sc_buf.h"
#include "sc/sc_queue.h"
#include <json.h>
#include <stdint.h>
#include <sys/socket.h>

#define EVENT_MAGIC 0x80000000 // Got this from sway-ipc(7)

enum ipc_message_type
{
    IPC_MESSAGE_GET_HISTORY_SIZE = 0,
    IPC_MESSAGE_SUBSCRIBE,
    IPC_MESSAGE_GET_ENTRY,
    N_IPC_REQUESTS,

    IPC_MESSAGE_ENTRY_ADDED = EVENT_MAGIC,
    IPC_MESSAGE_ENTRY_DELETED = EVENT_MAGIC + 2,
    IPC_MESSAGE_ENTRY_UPDATED = EVENT_MAGIC + 4,
    IPC_EVENT_LAST = IPC_MESSAGE_ENTRY_UPDATED
};

struct ipc_message
{
    enum ipc_message_type type;
    struct json_object   *msg;

    // Auxillary fd for this message, -1 if not set.
    int aux_fd;
};

struct ipc_write
{
    struct sc_buf buf;
    bool          sent_dummy;
    int           aux_fd;
};
sc_queue_def(struct ipc_write, ipc_write);

// Note that "aux_fd" will not be closed after callback
typedef void (*ipc_msg_callback)(struct ipc_message *msg, void *udata);

struct ipc_ct
{
    int fd;

    uint32_t hdr[2]; // type, size
    int      hdr_len;
    int      aux_fd;
    bool     got_dummy;

    uint32_t             remaining;
    struct json_tokener *tokener;

    char buf[4096]; // Used for I/O operations
    int  len;

    struct sc_queue_ipc_write write_queue;

    ipc_msg_callback callback;
    void            *callback_udata;
};

// clang-format off
bool ipc_ct_init(struct ipc_ct *ict, int fd, ipc_msg_callback callback, void *udata);
void ipc_ct_uninit(struct ipc_ct *ict);
int ipc_ct_process(struct ipc_ct *ict, int revents, bool poll);
bool ipc_ct_write_msg(struct ipc_ct *ict, enum ipc_message_type type, struct json_object *msg, int aux_fd);
// clang-format on
