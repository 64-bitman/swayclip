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

#define IPC_REQ_SUBSCRIBE "subscribe"
#define IPC_REQ_GET_HISTORY_LENGTH "get_history_length"
#define IPC_REQ_GET_ENTRIES "get_entries"
#define IPC_REQ_GET_DATA "get_data"
#define IPC_REQ_SET_SELECTION "set_selection"
#define IPC_REQ_DELETE_ENTRY "delete_entry"
#define IPC_REQ_PIN_ENTRY "pin_entry"

#define IPC_EVENT_ENTRY_ADD "entry_add"
#define IPC_EVENT_FLAG_ENTRY_ADD 1
#define IPC_EVENT_ENTRY_DELETE "entry_delete"
#define IPC_EVENT_FLAG_ENTRY_DELETE 2
#define IPC_EVENT_ENTRY_UPDATE "entry_update"
#define IPC_EVENT_FLAG_ENTRY_UPDATE 4
#define IPC_EVENT_CLIPBOARD_STATE "clipboard_state"
#define IPC_EVENT_FLAG_CLIPBOARD_STATE 8

enum ipc_message_type
{
    IPC_MESSAGE_CALL = 0,
    IPC_MESSAGE_EVENT
};

// Messages are in the format of <type><payload size><payload>, where <type> is
// an unsigned byte and <payload size> is an unsigned 32 bit integer in native
// byte order. The message may have an associated file descriptor with it using
// SCM_RIGHTS. This fd will be mapped to "aux_data" with the length of it being
// "aux_data_len".
struct ipc_message
{
    enum ipc_message_type type;
    struct json_object   *payload;
    void                 *aux_data; // May be NULL
    size_t                aux_data_len;
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

    enum ipc_message_type pending_type;
    uint32_t              pending_size;
    int                   scm_fd;
    struct json_tokener  *tokener;
    uint8_t               buf[4096];

    struct xarray_ipc_write write_queue;
};

typedef void (*ipc_msg_callback)(struct ipc_message *msg, void *udata);

// clang-format off
char *get_ipc_path(void);
bool ipc_ct_init(struct ipc_ct *ict, int fd);
void ipc_ct_uninit(struct ipc_ct *ict);
bool ipc_ct_read(struct ipc_ct *ict, bool need_scm, ipc_msg_callback callback, void *udata);
bool ipc_ct_write(struct ipc_ct *ict);
bool ipc_ct_has_pending_writes(struct ipc_ct *ict);
void ipc_ct_write_msg(struct ipc_ct *ict, enum ipc_message_type type, struct json_object *msg, int scm_fd);
// clang-format on
