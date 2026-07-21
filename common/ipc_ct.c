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
#include "event.h"
#include "io.h"
#include "log.h"
#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

#define HDR_SIZE ((int)sizeof(uint32_t) * 2)

/*
 * "fd" should be non blocking
 */
bool
ipc_ct_init(struct ipc_ct *ict, int fd, ipc_msg_callback callback, void *udata)
{
    ict->tokener = json_tokener_new();
    if (ict->tokener == NULL)
        return false;

    ict->fd = fd;
    ict->callback = callback;
    ict->callback_udata = udata;

    ict->hdr_len = 0;
    ict->aux_fd = -1;
    ict->got_dummy = false;

    sc_queue_init(&ict->write_queue);

    return true;
}

/*
 * Note that this closes the fd
 */
void
ipc_ct_uninit(struct ipc_ct *ict)
{
    struct ipc_write wr;

    sc_queue_foreach(&ict->write_queue, wr)
    {
        sc_buf_term(&wr.buf);
        if (wr.aux_fd != -1)
            close(wr.aux_fd);
    }
    if (ict->aux_fd != -1)
        close(ict->aux_fd);
    sc_queue_term(&ict->write_queue);
    close(ict->fd);
    json_tokener_free(ict->tokener);
}

static bool
ipc_ct_read(struct ipc_ct *ict)
{
    while (true)
    {
        if (!ict->got_dummy)
        {
            bool again = false;

            if (ict->aux_fd != -1)
                close(ict->aux_fd);
            if (!io_recv_fd(ict->fd, &ict->aux_fd, &again))
                return false;
            if (again)
                // Poll for more data
                return true;
            ict->got_dummy = true;
            continue;
        }
        if (ict->hdr_len < HDR_SIZE)
        {
            while (true)
            {
                ssize_t r = read(
                    ict->fd,
                    (uint8_t *)ict->hdr + ict->hdr_len,
                    HDR_SIZE - ict->hdr_len
                );

                if (r == -1)
                {
                    if (errno == EINTR)
                        continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        return true;
                    log_errerror("Error reading IPC header");
                    return false;
                }
                else if (r == 0)
                    // EOF received
                    return false;

                ict->hdr_len += r;
                break;
            }

            // Payload must be smaller than 64 KiB
            if (ict->hdr_len == HDR_SIZE && ict->hdr[1] > 65536)
            {
                // I guess just kill this connection...
                log_error("IPC message size is too large");
                return false;
            }
            continue;
        }

        bool valid = ict->hdr[1] > 0; // Ignore messages with zero payload
                                      // size
        ssize_t r;

        if (valid)
        {
            r = read(ict->fd, ict->buf, MIN(sizeof(ict->buf), ict->hdr[1]));

            if (r == -1)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return true;
                log_errerror("Error reading IPC payload");
                return false;
            }
            else if (r == 0)
                return false;

            ict->hdr[1] -= r;
        }

        enum json_tokener_error j_err = json_tokener_error_size;
        struct json_object     *obj;

        if (valid)
        {
            obj = json_tokener_parse_ex(ict->tokener, ict->buf, r);
            j_err = json_tokener_get_error(ict->tokener);
        }

        // If parsing error, then just consume the rest of the message.
        if (j_err == json_tokener_success || ict->hdr[1] == 0)
        {
            if (j_err == json_tokener_success)
            {
                struct ipc_message msg = {
                    .msg = obj,
                    .type = (enum ipc_message_type)ict->hdr[0],
                    .aux_fd = ict->aux_fd
                };

                ict->callback(&msg, ict->callback_udata);
            }

            ict->hdr_len = 0;
            ict->got_dummy = false;
            ict->aux_fd = -1;
        }
        else if (j_err == json_tokener_continue)
            continue;
        else if (valid)
            log_warn(
                "Error parsing JSON message: %s", json_tokener_error_desc(j_err)
            );
    }
}

static bool
ipc_ct_write(struct ipc_ct *ict, bool *pollout)
{
    *pollout = true;

    while (true)
    {
        if (sc_queue_empty(&ict->write_queue))
        {
            *pollout = false;
            return true;
        }

        struct ipc_write *wr = &sc_queue_peek_first(&ict->write_queue);

        if (!wr->sent_dummy)
        {
            bool again = false;

            if (!io_send_fd(ict->fd, wr->aux_fd, &again))
                return false;
            if (again)
                // Wait until socket is writable again
                return true;
            wr->sent_dummy = true;
            continue;
        }

        ssize_t w =
            write(ict->fd, sc_buf_rbuf(&wr->buf), sc_buf_size(&wr->buf));

        if (w == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            log_errerror("Error reading IPC payload");
            return false;
        }
        else if (w == 0)
            return false;

        sc_buf_mark_read(&wr->buf, w);

        if (sc_buf_size(&wr->buf) == 0)
        {
            sc_buf_term(&wr->buf);
            (void)sc_queue_del_first(&ict->write_queue);
        }
    }
}

/*
 * Process any ingoing and outgoing messages. Assume errors are fatal.
 */
bool
ipc_ct_process(struct ipc_ct *ict, int revents, bool poll, bool *need_pollout)
{
    int pollin = poll ? POLLIN : EPOLLIN;
    int pollout = poll ? POLLOUT : EPOLLOUT;

    if (revents & pollin && !ipc_ct_read(ict))
        return false;

    if (revents & pollout)
    {
        if (!ipc_ct_write(ict, need_pollout))
            return false;
    }
    return true;
}

/*
 * Note that ownership of "msg" and "aux_fd" is taken (even on failure).
 */
bool
ipc_ct_write_msg(
    struct ipc_ct        *ict,
    enum ipc_message_type type,
    struct json_object   *msg,
    int                   aux_fd
)
{
    struct ipc_write wr = {0};

    bool res = false;

    size_t      len;
    const char *str =
        json_object_to_json_string_length(msg, JSON_C_TO_STRING_PLAIN, &len);

    if (len > (size_t)UINT32_MAX)
        goto exit;

    sc_buf_init(&wr.buf, 128);

    // Don't add dummy byte here, that will be sent when message is actually
    // written to socket.
    sc_buf_put_32(&wr.buf, (uint32_t)type);
    sc_buf_put_32(&wr.buf, (uint32_t)len);
    sc_buf_put_raw(&wr.buf, str, len);

    if (!sc_buf_valid(&wr.buf))
    {
        sc_buf_term(&wr.buf);
        goto exit;
    }

    wr.aux_fd = aux_fd;
    sc_queue_add_last(&ict->write_queue, wr);

    res = true;
exit:
    json_object_put(msg);
    return res;
}
