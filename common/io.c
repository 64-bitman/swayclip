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

#include "io.h"
#include "log.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

/*
 * Get time in nanoseconds depending on clock ID. I guess this is IO related? I
 * dont fucking know...
 */
int64_t
get_time_ns(clockid_t id)
{
    struct timespec ts;

    if (clock_gettime(id, &ts) == -1)
    {
        log_errwarn("Error getting time");
        return -1;
    }

    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * Make the given fd non blocking. Returns true on success and false on failure.
 */
bool
set_fd_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        log_errwarn("Error making fd non-blocking");
        return false;
    }
    return true;
}

/*
 * Start reading data from the context, with the given timeout in milliseconds.
 * If timed out, then false is returned.
 */
bool
io_read(struct io_read *ctx, int timeout)
{
    struct pollfd pfd = {.fd = ctx->fd, .events = POLLIN};

    sc_array_init(&ctx->arr);

    while (true)
    {
        int ret = poll(&pfd, 1, timeout);

        if (ret <= 0)
        {
            if (ret == -1)
                log_errerror("Error polling fd %d", ctx->fd);
            else
                log_errerror("Timeout out reading from fd %d", ctx->fd);
            goto fail;
        }
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        else if (!(pfd.revents & POLLIN))
            continue;

        ssize_t r = read(ctx->fd, ctx->buf, ctx->bufsize);

        if (r == -1)
        {
            // Assume fatal
            log_errerror("Error reading data");
            goto fail;
        }
        else if (r > 0)
        {
            sc_array_concat(&ctx->arr, ctx->buf, r);
            if (sc_array_oom(&ctx->arr))
            {
                log_errerror("Out of memory!");
                goto fail;
            }
            ctx->data_callback(ctx->buf, r, ctx->callback_udata);
        }
        else
            // EOF received
            break;
    }

    return true;
fail:
    sc_array_term(&ctx->arr);
    return false;
}
