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

#include "ipc.h"
#include "common/io.h"
#include "common/ipc_ct.h"
#include "common/json_util.h"
#include "common/log.h"
#include "common/xdg.h"
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct ipc_client
{
    // Bitflag of events this client has subscribed to
    int events;

    struct ipc   *ipc;
    struct ipc_ct ict;

    struct xlist_ipc_client link;
};
xlist_define(ipc_client, struct ipc_client, link);

static void ipc_client_free(struct ipc_client *client);

static void
message_callback(struct ipc_message *msg, void *udata)
{
    struct ipc_client *client = udata;

    client->ipc->callback(client, msg, client->ipc->callback_udata);
    (void)eventloop_mod(client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
}

static bool
client_callback(int fd, int events, void *udata)
{
    struct ipc_client *client = udata;

    if (events == 0)
        return false;

    bool ret = true;

    // This logic to change fd events is kinda convoluted, is there a better
    // way?
    if (events & EPOLLIN)
        ret = ipc_ct_read(&client->ict, false, message_callback, client);
    if (ret && events & EPOLLOUT)
        ret = ipc_ct_write(&client->ict);

    if (!ret || events & (EPOLLHUP | EPOLLERR))
        goto stop;

    // If there is nothing to write, stop polling for write events.
    if (!ipc_ct_has_pending_writes(&client->ict) &&
        !eventloop_mod(client->ipc->loop, fd, EPOLLIN))
        goto stop;

    return false;
stop:
    ipc_client_free(client);
    return true;
}

static bool
ipc_add_client(struct ipc *ipc, int client_fd)
{
    struct ipc_client *client = calloc(1, sizeof(*client));

    if (client == NULL)
        return false;

    if (!set_fd_nonblocking(client_fd))
        return false;

    if (!ipc_ct_init(&client->ict, client_fd))
    {
        free(client);
        return false;
    }
    if (!eventloop_add(
            ipc->loop,
            client_fd,
            EVENT_PRIORITY_NORMAL,
            EPOLLIN,
            client_callback,
            client
        ))
    {
        ipc_ct_uninit(&client->ict);
        free(client);
        return false;
    }

    client->ipc = ipc;
    xlist_insert_after_ipc_client(&ipc->connections, client);

    log_debug("New IPC client");

    return true;
}

static void
ipc_client_free(struct ipc_client *client)
{
    eventloop_del(client->ipc->loop, client->ict.fd);
    ipc_ct_uninit(&client->ict);

    log_debug("IPC client closed");

    xlist_unlink_ipc_client(client);
    free(client);
}

static bool
accept_callback(int fd, int events, void *udata)
{
    if (events & (EPOLLHUP | EPOLLERR))
        return true;
    if (!(events & EPOLLIN))
        return false;

    struct ipc *ipc = udata;

    int client_fd = accept(fd, NULL, NULL);

    if (client_fd == -1)
    {
        log_errwarn("Error accepting IPC client");
        return false;
    }

    if (!ipc_add_client(ipc, client_fd))
        close(client_fd);

    return false;
}

bool
ipc_init(
    struct ipc          *ipc,
    struct eventloop    *loop,
    ipc_request_callback callback,
    void                *udata
)
{
    char *path;
    char *lock_path;

    const char *socket_path = getenv("SWAYCLIP_SOCK");

    if (socket_path == NULL)
    {
        char *dir = xdg_get_base_dir(XDG_RUNTIME_DIR, NULL);

        if (dir == NULL)
            return false;
        if (mkdir(dir, 0755) == -1 && errno != EEXIST)
        {
            log_errerror("Error creating directory '%s'", dir);
            free(dir);
            return false;
        }

        const char *display = getenv("WAYLAND_DISPLAY");

        if (display == NULL)
        {
            // Shouldn't happen, because we initialize wayland connection before
            // this.
            free(dir);
            return false;
        }

        // Get basename of display in case it is a path
        const char *s = strrchr(display, '/');
        if (s != NULL)
            display = s + 1;

        path = xstrdup_printf("%s/swayclip.%s", dir, display);
        lock_path = xstrdup_printf("%s/swayclip.%s.lock", dir, display);
        free(dir);
    }
    else
    {
        path = strdup(socket_path);
        lock_path = xstrdup_printf("%s.lock", socket_path);
    }

    // Check if socket exists. If so, then check if it is actually used by a
    // process, and if so, delete it.
    pid_t pid = lock_is_locked(lock_path);

    if (pid == 0)
        goto fail;
    else if (pid != -1)
    {
        log_error(
            "Error starting IPC server, process %d owns socket path", pid
        );
        goto fail;
    }
    else
    {
        unlink(path);
        unlink(lock_path);
    }

    if (!create_lock(lock_path, &ipc->lock_fd))
        goto fail;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd == -1)
    {
        log_errerror("Error creating IPC socket");
        goto fail2;
    }

    struct sockaddr_un addr;

    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    addr.sun_family = AF_UNIX;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        log_errerror("Error binding to IPC socket");
        goto fail2;
    }

    if (listen(fd, 5) == -1)
    {
        log_errerror("Error listening to IPC socket");
        goto fail2;
    }

    if (!set_fd_nonblocking(fd))
        goto fail2;

    ipc->loop = loop;

    if (!eventloop_add(
            loop, fd, EVENT_PRIORITY_NORMAL, EPOLLIN, accept_callback, ipc
        ))
        goto fail2;

    ipc->path = path;
    ipc->lock_path = lock_path;
    ipc->fd = fd;
    xlist_init_ipc_client(&ipc->connections);

    ipc->callback = callback;
    ipc->callback_udata = udata;

    return true;
fail2:
    unlink(lock_path);
    close(fd);
    close(ipc->lock_fd);
fail:
    free(path);
    free(lock_path);
    return false;
}

void
ipc_uninit(struct ipc *ipc)
{
    eventloop_del(ipc->loop, ipc->fd);

    struct ipc_client *client;

    xlist_foreach_safe(ipc_client, &ipc->connections, client)
    {
        ipc_client_free(client);
    }

    close(ipc->fd);
    close(ipc->lock_fd);
    unlink(ipc->path);
    unlink(ipc->lock_path);
    free(ipc->path);
    free(ipc->lock_path);
}

void
ipc_emit_event(struct ipc *ipc, struct json_object *obj)
{
    struct ipc_client *client;

    xlist_foreach(ipc_client, &ipc->connections, client) {}
    json_object_put(obj);
}

void
ipc_client_send_error(struct ipc_client *client, const char *desc)
{
}

void
ipc_client_add_success(struct ipc_client *client)
{
}
