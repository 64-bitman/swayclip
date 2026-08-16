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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// Events are always emitted after a IPC request.
struct pending_event
{
    struct json_object *obj;
};
xarray_create(struct pending_event, event, uint32_t, 2, 2);

struct ipc_client
{
    // Bitflag of events this client has subscribed to
    uint events;

    // Number of requests to hold before executing all of them at once. zero
    // means execute immediately
    uint32_t                  hold;
    struct xarray_ipc_message hold_arr;

    struct xarray_event pending_events;
    bool                handling_req; // If handling request

    struct ipc   *ipc;
    struct ipc_ct ict;

    struct xlist_ipc_client link;
};
xlist_define(ipc_client, struct ipc_client, link);

static void ipc_client_free(struct ipc_client *client);

static void
ipc_client_flush_events(struct ipc_client *client)
{
    xarray_foreach(&client->pending_events, i)
    {
        struct pending_event ev = xarray_val_event(&client->pending_events, i);

        ipc_ct_write_msg(&client->ict, IPC_MESSAGE_EVENT, ev.obj, -1);
        (void)eventloop_mod(
            client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT
        );
    }
    xarray_clear_event(&client->pending_events);
}

static void
message_callback(struct ipc_message *imsg, void *udata)
{
    struct ipc_client *client = udata;

    if (imsg->type != IPC_MESSAGE_REQUEST)
        goto exit;

    if (client->hold > 0)
    {
        if (!xarray_add_ipc_message(&client->hold_arr, *imsg))
            goto exit;
        if (--client->hold == 0)
        {
            xarray_foreach(&client->hold_arr, i)
            {
                imsg = xarray_ptr_ipc_message(&client->hold_arr, i);

                client->handling_req = true;
                client->ipc->callback(
                    client, imsg, client->ipc->callback_udata
                );
                client->handling_req = false;
                ipc_message_clear(imsg);
            }
            xarray_clear_ipc_message(&client->hold_arr);
        }
        goto flush;
    }

    client->handling_req = true;
    client->ipc->callback(client, imsg, client->ipc->callback_udata);
    client->handling_req = false;

exit:
    ipc_message_clear(imsg);
flush:
    ipc_client_flush_events(client);
}

static bool
client_callback(int fd, int events, void *udata)
{
    struct ipc_client *client = udata;

    if (events == 0)
        return false;

    bool ret = true;

    if (events & EPOLLIN)
        ret = ipc_ct_read(
            &client->ict, IPC_CT_TAKE_OWNERSHIP, message_callback, client
        );
    if (ret && events & EPOLLOUT)
    {
        bool poll = false;

        ret = ipc_ct_write(&client->ict, &poll);
        if (!ret && poll)
            ret = true;
    }

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
    xarray_init_ipc_message(&client->hold_arr);
    xarray_init_event(&client->pending_events);
    xlist_insert_after_ipc_client(&ipc->connections, client);

    log_debug("New IPC client");

    return true;
}

static void
ipc_client_free(struct ipc_client *client)
{
    log_debug("IPC client closed");

    eventloop_del(client->ipc->loop, client->ict.fd);
    ipc_ct_uninit(&client->ict);

    xarray_foreach(&client->hold_arr, i)
        ipc_message_clear(xarray_ptr_ipc_message(&client->hold_arr, i));

    xarray_foreach(&client->pending_events, i)
        json_object_put(xarray_ptr_event(&client->pending_events, i)->obj);

    xarray_uninit_ipc_message(&client->hold_arr);
    xarray_uninit_event(&client->pending_events);
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

    path = get_ipc_path();
    if (path == NULL)
        return false;

    lock_path = xstrdup_printf("%s.lock", path);
    if (lock_path == NULL)
    {
        log_errerror("Error allocating IPC lock path");
        free(path);
        return false;
    }

    // Check if socket exists. If so, then check if it is actually used by a
    // process, and if not, delete it.
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

    struct sockaddr_un addr = {0};

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

    log_debug("Initialized IPC server at \"%s\"", path);

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
        ipc_client_free(client);

    close(ipc->fd);
    close(ipc->lock_fd);
    unlink(ipc->path);
    unlink(ipc->lock_path);
    free(ipc->path);
    free(ipc->lock_path);
}

static void
ipc_emit_event(
    struct ipc *ipc, const char *str, uint event, struct json_object *msg
)
{
    struct ipc_client *client;

    if (msg == NULL)
        return;

    log_debug("Emitting event \"%s\"", str);

    xlist_foreach(ipc_client, &ipc->connections, client)
    {
        if (client->events & event)
        {
            if (client->handling_req)
                xarray_add_event(
                    &client->pending_events,
                    (struct pending_event){.obj = json_object_get(msg)}
                );
            else
                ipc_ct_write_msg(
                    &client->ict, IPC_MESSAGE_EVENT, json_object_get(msg), -1
                );
            (void)eventloop_mod(ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
        }
    }
    json_object_put(msg);
}

static bool
ipc_event_subscribed(struct ipc *ipc, uint event)
{

    struct ipc_client *client;

    xlist_foreach(ipc_client, &ipc->connections, client)
    {
        if (client->events & event)
            return true;
    }
    return false;
}

void
ipc_event_entry_add(struct ipc *ipc, int64_t entry_id)
{
    if (!ipc_event_subscribed(ipc, IPC_EVENT_FLAG_ENTRY_ADD))
        return;
    ipc_emit_event(
        ipc,
        IPC_EVENT_ENTRY_ADD,
        IPC_EVENT_FLAG_ENTRY_ADD,
        build_json_object(
            NULL,
            -1,
            JSON_FIELD_STR("event", IPC_EVENT_ENTRY_ADD),
            JSON_FIELD_INT("id", entry_id),
            NULL
        )
    );
}

/*
 * If -1 is passed for "entry_id", then all entries are considered to be deleted
 * (history cleared).
 */
void
ipc_event_entry_delete(struct ipc *ipc, int64_t entry_id)
{
    if (!ipc_event_subscribed(ipc, IPC_EVENT_FLAG_ENTRY_DELETE))
        return;
    ipc_emit_event(
        ipc,
        IPC_EVENT_ENTRY_DELETE,
        IPC_EVENT_FLAG_ENTRY_DELETE,
        build_json_object(
            NULL,
            -1,
            JSON_FIELD_STR("event", IPC_EVENT_ENTRY_DELETE),
            JSON_FIELD_INT("id", entry_id),
            NULL
        )
    );
}

/*
 * "pinned" and "update_time" are optional, but at least one should be set.
 */
void
ipc_event_entry_update(
    struct ipc    *ipc,
    int64_t        entry_id,
    const int64_t *update_time,
    const bool    *pinned
)
{
    if (!ipc_event_subscribed(ipc, IPC_EVENT_FLAG_ENTRY_UPDATE))
        return;

    struct json_object *msg;

    msg = build_json_object(
        NULL,
        -1,
        JSON_FIELD_STR("event", IPC_EVENT_ENTRY_UPDATE),
        JSON_FIELD_INT("id", entry_id),
        NULL
    );

    if (pinned != NULL)
        build_json_object(msg, -1, JSON_FIELD_BOOL("pinned", *pinned), NULL);
    if (update_time != NULL)
        build_json_object(
            msg, -1, JSON_FIELD_INT("update_time", *update_time), NULL
        );
    ipc_emit_event(
        ipc, IPC_EVENT_ENTRY_UPDATE, IPC_EVENT_FLAG_ENTRY_UPDATE, msg
    );
}

void
ipc_event_entry_move(
    struct ipc *ipc, int64_t entry_id, int64_t old_pos, int64_t new_pos
)
{
    if (!ipc_event_subscribed(ipc, IPC_EVENT_FLAG_ENTRY_MOVE))
        return;
    ipc_emit_event(
        ipc,
        IPC_EVENT_ENTRY_MOVE,
        IPC_EVENT_FLAG_ENTRY_MOVE,
        build_json_object(
            NULL,
            -1,
            JSON_FIELD_STR("event", IPC_EVENT_ENTRY_MOVE),
            JSON_FIELD_INT("id", entry_id),
            JSON_FIELD_INT("old_pos", old_pos),
            JSON_FIELD_INT("new_pos", new_pos),
            NULL
        )
    );
}

/*
 * Emit event when entry is set as the current entry or not.
 */
void
ipc_event_entry_state(struct ipc *ipc, int64_t entry_id, bool state)
{
    if (!ipc_event_subscribed(ipc, IPC_EVENT_FLAG_ENTRY_STATE))
        return;
    ipc_emit_event(
        ipc,
        IPC_EVENT_ENTRY_STATE,
        IPC_EVENT_FLAG_ENTRY_STATE,
        build_json_object(
            NULL,
            -1,
            JSON_FIELD_STR("event", IPC_EVENT_ENTRY_STATE),
            JSON_FIELD_INT("id", entry_id),
            JSON_FIELD_BOOL("state", state),
            NULL
        )
    );
}

void
ipc_client_send(struct ipc_client *client, struct json_object *msg, int scm_fd)
{
    if (msg == NULL)
        return;

    ipc_ct_write_msg(&client->ict, IPC_MESSAGE_RESPONSE, msg, scm_fd);
    (void)eventloop_mod(client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
}

void
ipc_client_send_error(struct ipc_client *client, const char *desc_fmt, ...)
{
    static char buf[256];
    va_list     ap;

    va_start(ap, desc_fmt);
    vsnprintf(buf, sizeof(buf), desc_fmt, ap);
    va_end(ap);

    ipc_ct_write_msg(
        &client->ict,
        IPC_MESSAGE_ERROR,
        build_json_object(NULL, 1, JSON_FIELD_STR("desc", buf), NULL),
        -1
    );
    (void)eventloop_mod(client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
}

void
ipc_client_send_success(struct ipc_client *client)
{
    ipc_ct_write_msg(
        &client->ict, IPC_MESSAGE_SUCCESS, build_json_object(NULL, -1, NULL), -1
    );
    (void)eventloop_mod(client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
}

void
ipc_client_send_success_fd(struct ipc_client *client, int scm_fd)
{
    ipc_ct_write_msg(
        &client->ict,
        IPC_MESSAGE_SUCCESS,
        build_json_object(NULL, -1, NULL),
        scm_fd
    );
    (void)eventloop_mod(client->ipc->loop, client->ict.fd, EPOLLIN | EPOLLOUT);
}

void
ipc_client_set_events(struct ipc_client *client, uint events)
{
    client->events = events;
}

void
ipc_client_add_hold(struct ipc_client *client, uint32_t n)
{
    client->hold += n;
}
