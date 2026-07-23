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

#include "ipc_ct.h"
#include "log.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define HEADER_SIZE ((ssize_t)(1 + sizeof(uint32_t)))

/*
 * "fd" should be non blocking
 */
bool
ipc_ct_init(struct ipc_ct *ict, int fd)
{
    ict->tokener = json_tokener_new();
    if (ict->tokener == NULL)
        return false;

    ict->fd = fd;
    ict->got_header = false;
    ict->scm_fd = -1;

    sc_array_init(&ict->write_queue);

    return true;
}

/*
 * Note that this closes the fd
 */
void
ipc_ct_uninit(struct ipc_ct *ict)
{
    struct ipc_write wr;

    sc_array_foreach(&ict->write_queue, wr) free(wr.data);
    sc_array_term(&ict->write_queue);

    close(ict->fd);
    json_tokener_free(ict->tokener);
}

/*
 * Receive from the IPC socket, and return true on success or false on fatal
 * error.
 */
bool
ipc_ct_read(struct ipc_ct *ict, ipc_msg_callback callback, void *udata)
{
    return true;
}

/*
 * Write any pending messages to the IPC socket. Return true on success and
 * false on fatal error.
 */
bool
ipc_ct_write(struct ipc_ct *ict)
{
    while (true)
    {
        if (sc_array_size(&ict->write_queue) == 0)
            break;

        struct ipc_write *wr = sc_array_ptr(&ict->write_queue, 0);

        uint32_t off = wr->size - wr->remaining;
        ssize_t  w = write(ict->fd, wr->data + off, wr->remaining);

        if (w == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                // Poll until socket is writable again
                return true;
            log_error("Error writing to IPC connection");
            return false;
        }
        if (w == 0)
            // Not sure if this can happen, just return to poll I guess...
            return true;

        wr->remaining -= w;

        if (wr->remaining == 0)
        {
            free(wr->data);
            sc_array_del(&ict->write_queue, 0);
        }
    }
    return true;
}

bool
ipc_ct_has_pending_writes(struct ipc_ct *ict)
{
    return sc_array_size(&ict->write_queue) > 0;
}

/*
 * Note that ownership of "payload" is always taken. If "payload" is invalid,
 * nothing is done.
 */
void
ipc_ct_write_msg(
    struct ipc_ct *ict, enum ipc_message_type type, union ipc_payload payload
)
{

    if ((type == IPC_MESSAGE_BLOB && payload.blob.data == NULL) ||
        (type == IPC_MESSAGE_JSON && payload.json == NULL))
        return;

    uint8_t *payload_data;
    uint32_t payload_sz;

    if (type == IPC_MESSAGE_BLOB)
    {
        payload_data = payload.blob.data;
        payload_sz = payload.blob.size;
    }
    else
    {
        struct json_object *msg = payload.json;

        size_t      len;
        const char *str = json_object_to_json_string_length(
            msg, JSON_C_TO_STRING_PLAIN, &len
        );

        if (len > (size_t)UINT32_MAX)
        {
            json_object_put(msg);
            return;
        }

        payload_data = (uint8_t *)str;
        payload_sz = (uint32_t)len;
    }

    struct ipc_write wr = {
        .size = HEADER_SIZE + payload_sz,
        .remaining = wr.size,
    };

    wr.data = malloc(wr.size);

    if (wr.data == NULL)
    {
        log_errerror("Error sending IPC message");
        goto exit;
    }

    memcpy(wr.data, (uint8_t *)&type, 1);
    memcpy(wr.data + 1, &payload_sz, sizeof(payload_sz));
    memcpy(wr.data + HEADER_SIZE, payload_data, payload_sz);
    sc_array_add(&ict->write_queue, wr);

exit:
    if (type == IPC_MESSAGE_JSON)
        json_object_put(payload.json);
    else
        free(payload.blob.data);
}
