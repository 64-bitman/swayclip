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
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Get time in nanoseconds depending on clock ID. Not IO related but uhh..
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
        log_errerror("Error making fd %d non-blocking", fd);
        return false;
    }
    return true;
}

/*
 * Start reading data from the context, with the given timeout in milliseconds.
 * If timed out, then false is returned. "max_bytes" is the maximum amount of
 * bytes to be received until error (limited by UINT32_MAX).
 */
bool
io_read(struct io_read *ctx, int timeout, size_t max_bytes)
{
    struct pollfd pfd = {.fd = ctx->fd, .events = POLLIN};

    xarray_init_io(&ctx->data);

    while (true)
    {
        int ret = poll(&pfd, 1, timeout);

        if (ret <= 0)
        {
            if (ret == -1)
            {
                if (errno == EINTR)
                    continue;
                log_errerror("Error polling fd %d", ctx->fd);
            }
            else
                log_errerror("Timed out reading from fd %d", ctx->fd);
            goto fail;
        }
        if (!(pfd.revents & POLLIN) &&
            pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            if (xarray_len_io(&ctx->data) == 0)
                goto fail;
            break;
        }
        else if (!(pfd.revents & POLLIN))
            continue;

        ssize_t r = read(ctx->fd, ctx->buf, ctx->bufsize);
        size_t  total = xarray_len_io(&ctx->data) + r;

        if (total > max_bytes || total > UINT32_MAX)
        {
            log_error("Data too large!");
            goto fail;
        }

        if (r == -1)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            // Assume fatal
            log_errerror("Error reading data");
            goto fail;
        }
        else if (r > 0)
        {
            if (!xarray_concat_io(&ctx->data, ctx->buf, r))
            {
                log_errerror("Out of memory!");
                goto fail;
            }
            if (ctx->data_callback != NULL)
                ctx->data_callback(ctx->buf, r, ctx->callback_udata);
        }
        else
            // EOF received
            break;
    }

    return true;
fail:
    xarray_uninit_io(&ctx->data);
    return false;
}

/*
 * Receive "len" bytes from "fd" into "buf", handling EINTR. If EAGAIN or
 * EWOULDBLOCK is returned, then "poll" is set to true and -1 is returned, then
 * If the payload has ancillary data, then place the first fd in the control
 * message in "scm_fd" (only if its value is -1 and non-NULL). If "*scm_fd" is
 * not -1 or "scm_fd" is NULL, then any control message is ignored/truncated.
 * Return number of bytes read, 0 on EOF, or -1 on fatal error.
 */
ssize_t
io_recv(int fd, uint8_t *buf, size_t len, int *scm_fd, bool *need_poll)
{
    bool need_scm = scm_fd != NULL && *scm_fd == -1;

    // https://man7.org/tlpi/code/online/dist/sockets/scm_rights_recv.c.html
    union
    {
        char           buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg = {0};
    struct iovec  iov = {.iov_base = buf, .iov_len = len};
    struct msghdr msgh = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (need_scm)
    {
        msgh.msg_control = cmsg.buf;
        msgh.msg_controllen = sizeof(cmsg.buf);
    }

    ssize_t r;

    while (true)
    {
        r = recvmsg(fd, &msgh, 0);

        if (r == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *need_poll = true;
                return -1;
            }
            log_errerror("Error receiving message from fd %d", fd);
            return -1;
        }
        else if (r == 0)
            // EOF receiveda
            return 0;
        break;
    }

    if (need_scm && msgh.msg_flags & MSG_TRUNC)
        // Bad client
        log_warn("Received socket control message truncated?");

    struct cmsghdr *chdr = need_scm ? CMSG_FIRSTHDR(&msgh) : NULL;

    if (need_scm && chdr != NULL)
    {
        if (chdr->cmsg_len != CMSG_LEN(sizeof(int)) ||
            chdr->cmsg_level != SOL_SOCKET || chdr->cmsg_type != SCM_RIGHTS)
        {
            log_warn("Received socket control message is invalid");
            return r;
        }

        memcpy(scm_fd, CMSG_DATA(chdr), sizeof(*scm_fd));
    }

    return r;
}

ssize_t
io_send(int fd, uint8_t *buf, size_t len, int scm_fd, bool *need_poll)
{
    union
    {
        char           buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg = {0};
    struct iovec  iov = {.iov_base = buf, .iov_len = len};
    struct msghdr msgh = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (scm_fd != -1)
    {
        msgh.msg_control = cmsg.buf;
        msgh.msg_controllen = sizeof(cmsg.buf);

        struct cmsghdr *chdr = CMSG_FIRSTHDR(&msgh);

        chdr->cmsg_len = CMSG_LEN(sizeof(int));
        chdr->cmsg_level = SOL_SOCKET;
        chdr->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(chdr), &scm_fd, sizeof(scm_fd));
    }

    ssize_t w;

    while (true)
    {
        w = sendmsg(fd, &msgh, 0);

        if (w == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *need_poll = true;
                return -1;
            }
            log_errerror("Error sending message to fd %d", fd);
            return -1;
        }
        else if (w == 0)
            // I guess exit?
            return -1;
        break;
    }

    return w;
}

/*
 * Create a lock file at the given path, and store its file descriptor in
 * "lock_fd". Returns OK on success and FAIL on failure.
 */
int
create_lock(const char *path, int *lock_fd)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);

    if (fd == -1)
    {
        log_errerror("Error creating lock file '%s'", path);
        return false;
    }

    struct flock fl;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = fl.l_len = 0;

    if (fcntl(fd, F_SETLK, &fl) == -1)
    {
        log_errerror("Error locking file '%s'", path);
        close(fd);
        return false;
    }

    *lock_fd = fd;
    return true;
}

/*
 * Returns locking PID if file is locked, otherwise -1 if unlocked or if it
 * doesn't exist. Returns 0 if an error occured.
 */
pid_t
lock_is_locked(const char *path)
{
    chmod(path, 0644);
    int fd = open(path, O_RDWR);

    if (fd == -1)
    {
        if (errno == ENOENT)
            return -1;
        else
        {
            log_errerror("Error opening file '%s'", path);
            return 0;
        }
    }

    struct flock fl;
    int          ret;

    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = fl.l_len = 0;

    ret = fcntl(fd, F_GETLK, &fl);
    close(fd);

    if (ret != -1)
        return fl.l_type == F_WRLCK ? fl.l_pid : -1;
    return 0;
}
