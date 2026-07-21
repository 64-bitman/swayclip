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
        log_errerror("Error making fd %d non-blocking", fd);
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

    sc_buf_init(&ctx->data, 4096);

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
            if (sc_buf_size(&ctx->data) == 0)
                goto fail;
            break;
        }
        else if (!(pfd.revents & POLLIN))
            continue;

        ssize_t r = read(ctx->fd, ctx->buf, ctx->bufsize);

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
            if (!ctx->no_data)
            {
                sc_buf_put_raw(&ctx->data, ctx->buf, r);
                if (!sc_buf_valid(&ctx->data))
                {
                    log_errerror("Out of memory!");
                    goto fail;
                }
            }
            ctx->data_callback(ctx->buf, r, ctx->callback_udata);
        }
        else
            // EOF received
            break;
    }

    return true;
fail:
    sc_buf_term(&ctx->data);
    return false;
}

/*
 * Write to the context with timeout in milliseconds. "data_callback" should
 * return true and set "*len" to zero if finished.
 */
bool
io_write(struct io_write *ctx, int timeout)
{
    uint8_t *ptr = ctx->buf;
    size_t   len = 0;

    struct pollfd pfd = {.fd = ctx->fd, .events = POLLOUT};

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
                log_errerror("Timed out writing to fd %d", ctx->fd);
            return false;
        }
        if (!(pfd.revents & POLLOUT) &&
            pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            return false;
        else if (!(pfd.revents & POLLOUT))
            continue;

        if (len == 0)
        {
            ptr = ctx->buf;
            if (!ctx->data_callback(
                    ptr, ctx->bufsize, &len, ctx->callback_udata
                ))
                return false;
            if (len == 0)
                return true;
        }

        ssize_t w = write(ctx->fd, ptr, len);

        if (w <= 0)
        {
            if (w == -1 &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            log_errerror("Error writing data");
            return false;
        }

        ptr += w;
        len -= w;
    }
    return true;
}

/*
 * Read a single byte from the socket and return the first fd associated with
 * the control message. If there is no fd, then set "fd" to -1.
 */
bool
io_recv_fd(int sock, int *fd, bool *again)
{
    uint8_t      dummy = 0;
    struct iovec iov = {.iov_base = &dummy, .iov_len = 1};

    struct msghdr msgh = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    // https://man7.org/tlpi/code/online/dist/sockets/scm_rights_recv.c.html
    union
    {
        char           buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;

    msgh.msg_control = cmsg.buf;
    msgh.msg_controllen = sizeof(cmsg.buf);

    while (true)
    {
        ssize_t r = recvmsg(sock, &msgh, 0);

        if (r == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *again = true;
                return true;
            }
            log_errerror("Error receiving message from socket");
            return false;
        }
        break;
    }

    struct cmsghdr *cmsgp = CMSG_FIRSTHDR(&msgh);

    if (cmsgp == NULL || cmsgp->cmsg_len != CMSG_LEN(sizeof(int)) ||
        cmsgp->cmsg_level != SOL_SOCKET || cmsgp->cmsg_type != SCM_RIGHTS)
    {
        *fd = -1;
        return true;
    }

    memcpy(fd, CMSG_DATA(cmsgp), sizeof(int));
    return true;
}

/*
 * Send a dummy byte with a control message with "fd". If "fd" is -1, then send
 * no control message.
 */
bool
io_send_fd(int sock, int fd, bool *again)
{
    uint8_t      dummy = 0;
    struct iovec iov = {.iov_base = &dummy, .iov_len = 1};

    struct msghdr msgh = {0};

    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (fd != -1)
    {
        // https://man7.org/tlpi/code/online/dist/sockets/scm_rights_send.c.html
        union
        {
            char           buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsg = {0};

        msgh.msg_control = cmsg.buf;
        msgh.msg_controllen = sizeof(cmsg.buf);

        struct cmsghdr *cmsgp = CMSG_FIRSTHDR(&msgh);

        cmsgp->cmsg_len = CMSG_LEN(sizeof(int));
        cmsgp->cmsg_level = SOL_SOCKET;
        cmsgp->cmsg_type = SCM_RIGHTS;
        memcpy(CMSG_DATA(cmsgp), &fd, sizeof(int));
    }

    while (true)
    {
        ssize_t w = sendmsg(sock, &msgh, 0);

        if (w == -1)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                *again = true;
                return true;
            }
            log_errerror("Error receiving message from socket");
            return false;
        }
        break;
    }
    return true;
}

/*
 * Create a lock file at the given path, and store its file descriptor in
 * "lock_fd". Returns OK on success and FAIL on failure.
 */
int
create_lock(const char *path, int *lock_fd)
{
    int fd = open(path, O_RDWR | O_CREAT);

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
