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
#include "io.h"
#include "log.h"
#include "xdg.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define HEADER_SIZE (1 + (int)sizeof(uint32_t))

xarray_create(uint8_t, write, uint32_t, 128, 2.0);

char *
get_ipc_path(void)
{
    char *path;

    const char *socket_path = getenv("SWAYCLIP_SOCK");

    if (socket_path == NULL)
    {
        char *dir = xdg_get_base_dir(XDG_RUNTIME_DIR, NULL);

        if (dir == NULL)
        {
            log_error("$XDG_RUNTIME_DIR not set in environment");
            return NULL;
        }

        const char *display = getenv("WAYLAND_DISPLAY");

        if (display == NULL)
        {
            log_error("$WAYLAND_DISPLAY not set in environment");
            free(dir);
            return NULL;
        }

        // Get basename of display in case it is a path
        const char *s = strrchr(display, '/');
        if (s != NULL)
            display = s + 1;

        path = xstrdup_printf("%s/swayclip.%s", dir, display);
        free(dir);
    }
    else
        path = strdup(socket_path);

    if (path == NULL)
    {
        log_errerror("Error allocating IPC socket path");
        return NULL;
    }

    return path;
}

static void
ipc_ct_reset(struct ipc_ct *ict)
{
    ict->pending_header = true;
    ict->pending_size = HEADER_SIZE;
}

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
    ict->scm_fd = -1;
    ipc_ct_reset(ict);

    xarray_init_ipc_write(&ict->write_queue);

    return true;
}

/*
 * Note that this closes the fd
 */
void
ipc_ct_uninit(struct ipc_ct *ict)
{
    struct ipc_write *wr;

    xarray_foreach(ipc_write, &ict->write_queue, wr)
    {
        free(wr->data);
        if (wr->scm_fd != -1)
            close(wr->scm_fd);
    }
    xarray_uninit_ipc_write(&ict->write_queue);
    if (ict->scm_fd != -1)
        close(ict->scm_fd);

    // Caller may set this to -1 if they closed it beforehand
    if (ict->fd != -1)
        close(ict->fd);
    json_tokener_free(ict->tokener);
}

/*
 * Receive from the IPC socket, and return true on success or false on fatal
 * error. Call "callback" for each message received.
 */
bool
ipc_ct_read(
    struct ipc_ct *ict, uint flags, ipc_msg_callback callback, void *udata
)
{
    while (true)
    {
        ssize_t r;
        bool    poll = false;
        int    *scm_ptr = (flags & IPC_CT_WANT_SCM_FD) ? &ict->scm_fd : NULL;

        // Subtract one because a NUL terminator may possibly be required.
        r = io_recv(
            ict->fd,
            ict->buf +
                (ict->pending_header ? (HEADER_SIZE - ict->pending_size) : 0),
            MIN(sizeof(ict->buf) - 1, ict->pending_size),
            scm_ptr,
            &poll
        );

        if (r == -1)
            return poll;

        if (ict->pending_header)
        {
            ict->pending_size -= r;
            if (ict->pending_size > 0)
                return true;

            ict->pending_type = ict->buf[0];
            memcpy(&ict->pending_size, ict->buf + 1, sizeof(uint32_t));

            // Restrict message size to 1 MiB
            if (ict->pending_size > 1048576)
            {
                // I guess just kill the connection?
                log_error("IPC message size is larger than 1 MiB");
                ipc_ct_reset(ict); // Just to be consistent
                return false;
            }
            ict->pending_header = false;
            continue;
        }

        ict->pending_size -= r;

        if (ict->pending_size == 0)
        {
            // NUL terminate the string
            ict->buf[r] = NUL;
            r++; // Include NUL in length
            ipc_ct_reset(ict);
        }

        enum json_tokener_error j_err;
        struct json_object     *msg =
            json_tokener_parse_ex(ict->tokener, (char *)ict->buf, r);

        j_err = json_tokener_get_error(ict->tokener);
        if (j_err == json_tokener_success)
        {
            struct ipc_message imsg = {
                .type = ict->pending_type, .payload = msg
            };
            int scm_fd = ict->scm_fd;

            ict->scm_fd = -1;

            if (scm_fd != -1 && flags & IPC_CT_NO_MMAP)
            {
                imsg.aux_fd = scm_fd;
            }
            else if (scm_fd != -1 && !(flags & IPC_CT_NO_MMAP))
            {
                struct stat st;

                if (fstat(scm_fd, &st) != -1)
                {
                    imsg.aux_data = mmap(
                        NULL, st.st_size, PROT_READ, MAP_SHARED, scm_fd, 0
                    );
                    if (imsg.aux_data == MAP_FAILED || imsg.aux_data == NULL)
                    {
                        if (imsg.aux_data == MAP_FAILED)
                            log_errerror("Error mapping IPC message fd");
                        imsg.aux_data = NULL;
                    }
                    else
                        imsg.aux_data_len = st.st_size;
                }
                else
                    log_warn("Error querying size of IPC message fd");
                close(scm_fd);
            }
            else if (flags & IPC_CT_NO_MMAP)
            {
                imsg.aux_fd = -1;
            }
            else
            {
                imsg.aux_data = NULL;
                imsg.aux_data_len = 0;
            }

            callback(&imsg, udata);

            if (!(flags & IPC_CT_TAKE_OWNERSHIP))
            {
                json_object_put(msg);
                if (flags & IPC_CT_NO_MMAP)
                {
                    if (imsg.aux_fd != -1)
                        close(imsg.aux_fd);
                }
                else
                {
                    if (imsg.aux_data != NULL)
                        munmap(imsg.aux_data, imsg.aux_data_len);
                }
            }
        }
        else if (j_err == json_tokener_continue)
            continue;
        else
        {
            // Just reset the tokener so that message after this corrupt message
            // isn't affected.
            json_tokener_reset(ict->tokener);
            log_warn(
                "Invalid IPC JSON message: %s", json_tokener_error_desc(j_err)
            );
        }
    }

    return true;
}

/*
 * Write any pending messages to the IPC socket. SCM_RIGHTS fd will always be
 * sent with the header. Return true on success and false on fatal error (unless
 * "*poll" is set to true).
 */
bool
ipc_ct_write(struct ipc_ct *ict, bool *need_poll)
{
    while (true)
    {
        if (xarray_len_ipc_write(&ict->write_queue) == 0)
            break;

        struct ipc_write *wr = xarray_ptr_ipc_write(&ict->write_queue, 0);

        uint32_t off = wr->size - wr->remaining;
        ssize_t  w = io_send(
            ict->fd, wr->data + off, wr->remaining, wr->scm_fd, need_poll
        );

        if (w == -1)
        {
            if (!(*need_poll))
                log_error("Error writing to IPC connection");
            return false;
        }
        if (wr->scm_fd != -1)
        {
            close(wr->scm_fd);
            wr->scm_fd = -1; // Don't want to send fd mutliple times
        }
        if (w == 0)
        {
            // Not sure if this can happen, just return to poll I guess...
            *need_poll = true;
            return false;
        }

        wr->remaining -= w;

        if (wr->remaining == 0)
        {
            free(wr->data);
            xarray_del_ipc_write(&ict->write_queue, 0);
        }
    }
    return true;
}

bool
ipc_ct_has_pending_writes(struct ipc_ct *ict)
{
    return xarray_len_ipc_write(&ict->write_queue) > 0;
}

/*
 * Note that ownership of "payload" and "scm_fd" (if not -1) is always taken. If
 * "msg" is NULL, nothing is written. If "scm_fd" is not -1, then it sent along
 * the message over the socket using SCM_RIGHTS.
 */
bool
ipc_ct_write_msg(
    struct ipc_ct        *ict,
    enum ipc_message_type type,
    struct json_object   *msg,
    int                   scm_fd
)
{
    if (msg == NULL)
        goto fail;

    size_t      len;
    const char *str =
        json_object_to_json_string_length(msg, JSON_C_TO_STRING_PLAIN, &len);

    if (len > (size_t)UINT32_MAX)
    {
        log_error("IPC message too large to be sent");
        goto fail;
    }

    struct xarray_write buf;
    uint32_t            l = len;

    xarray_init_write(&buf);

    if (!xarray_set_size_write(&buf, HEADER_SIZE + len))
        goto fail;

    // Should never fail
    assert(type <= UINT8_MAX);
    xarray_add_write(&buf, type);
    xarray_concat_write(&buf, (uint8_t *)&l, sizeof(l));
    xarray_concat_write(&buf, (uint8_t *)str, len);

    struct ipc_write wr;

    wr.data = xarray_steal_write(&buf, &wr.size);
    wr.remaining = wr.size;
    wr.scm_fd = scm_fd;

    if (!xarray_add_ipc_write(&ict->write_queue, wr))
    {
        free(wr.data);
        goto fail;
    }
    json_object_put(msg);
    return true;
fail:
    json_object_put(msg);
    if (scm_fd != -1)
        close(scm_fd);
    return false;
}
