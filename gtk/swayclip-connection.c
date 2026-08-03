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

#include "swayclip-connection.h"

// clang-format off
G_DEFINE_BOXED_TYPE(SwayclipMessage, swayclip_message, swayclip_message_copy, swayclip_message_free)
// clang-format on

typedef struct
{
    struct json_object *obj;
    int                 scm_fd; // -1 if not set
} SwayclipRequest;

struct _SwayclipConnection
{
    GObject parent;

    char    *display;
    char    *sock_path;
    GThread *ipc_thread;

    // Used to wakeup thread
    GCancellable *cancel_poll;

    GAsyncQueue *request_queue; // SwayRequest objects waiting to be sent
    GAsyncQueue *pending_queue; // SwayMessage objects that have been received
    GQueue      *task_queue;    // Queue of GTask objects for pending requests

    guint  idle_id;
    GMutex idle_mut;

    gboolean stop;
    GMutex   stop_mut;
};

typedef enum
{
    SIGNAL_EVENT,
    N_SIGNALS
} SwayclipConnectionSignal;

static guint signals[N_SIGNALS] = {0};

G_DEFINE_TYPE(SwayclipConnection, swayclip_connection, G_TYPE_OBJECT)

static void
swayclip_connection_finalize(GObject *obj)
{
    SwayclipConnection *self = SWAYCLIP_CONNECTION(obj);

    g_free(self->display);
    g_free(self->sock_path);

    g_async_queue_unref(self->request_queue);
    g_async_queue_unref(self->pending_queue);
    g_queue_free_full(self->task_queue, g_object_unref);

    g_mutex_clear(&self->stop_mut);
    g_mutex_clear(&self->idle_mut);

    G_OBJECT_CLASS(swayclip_connection_parent_class)->finalize(obj);
}

static void
swayclip_connection_dispose(GObject *obj)
{
    SwayclipConnection *self = SWAYCLIP_CONNECTION(obj);

    if (self->ipc_thread != NULL)
    {
        g_mutex_lock(&self->stop_mut);
        self->stop = TRUE;
        g_mutex_unlock(&self->stop_mut);
        g_cancellable_cancel(self->cancel_poll);

        g_thread_join(self->ipc_thread);
        self->ipc_thread = NULL;
    }

    g_clear_object(&self->cancel_poll);

    G_OBJECT_CLASS(swayclip_connection_parent_class)->dispose(obj);
}

static void
swayclip_connection_class_init(SwayclipConnectionClass *class)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(class);

    obj_class->finalize = swayclip_connection_finalize;
    obj_class->dispose = swayclip_connection_dispose;

    signals[SIGNAL_EVENT] = g_signal_new(
        "event",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE,
        0,
        NULL,
        NULL,
        NULL,
        G_TYPE_NONE,
        1,
        SWAYCLIP_TYPE_MESSAGE
    );
}

static gboolean
message_idle_cb(SwayclipConnection *ct)
{
    g_mutex_lock(&ct->idle_mut);
    ct->idle_id = 0;
    g_mutex_unlock(&ct->idle_mut);

    SwayclipMessage *msg;

    while ((msg = g_async_queue_try_pop(ct->pending_queue)) != NULL)
    {
        if (msg->type == IPC_MESSAGE_EVENT)
        {
            g_signal_emit(ct, signals[SIGNAL_EVENT], 0, msg);
            continue;
        }
        else if (msg->type != IPC_MESSAGE_CALL)
        {
            swayclip_message_free(msg);
            continue;
        }

        g_autoptr(GTask) task = g_queue_pop_head(ct->task_queue);

        if (task == NULL)
        {
            // Should only happen if daemon is not working properly
            g_warning("Received response has no associated request");
            swayclip_message_free(msg);
            continue;
        }
        g_task_return_pointer(task, msg, (GDestroyNotify)swayclip_message_free);
    }

    return G_SOURCE_REMOVE;
}

static void
ipc_message_callback(struct ipc_message *imsg, void *udata)
{
    SwayclipConnection *ct = udata;
    SwayclipMessage    *msg = g_new0(SwayclipMessage, 1);

    msg->type = imsg->type;
    msg->obj = imsg->payload;

    if (imsg->aux_fd != -1)
    {
        g_autoptr(GError) error = NULL;

        msg->aux_data = g_mapped_file_new_from_fd(imsg->aux_fd, FALSE, &error);
        if (msg->aux_data == NULL)
            g_warning("Error mapping file descriptor: %s", error->message);
    }

    g_async_queue_push(ct->pending_queue, msg);

    g_mutex_lock(&ct->idle_mut);
    if (ct->idle_id == 0)
    {
        g_object_ref(ct);
        ct->idle_id = g_idle_add_full(
            G_PRIORITY_LOW, (GSourceFunc)message_idle_cb, ct, g_object_unref
        );
    }
    g_mutex_unlock(&ct->idle_mut);
}

static void *
ipc_thread_cb(SwayclipConnection *ct)
{
    g_autofree char *path = NULL;

    if (ct->sock_path == NULL)
    {
        if (ct->display == NULL)
        {
            g_warning(
                "$WAYLAND_DISPLAY not set in environment, can not connect to "
                "IPC server"
            );
            return NULL;
        }
        path = g_strdup_printf(
            "%s/swayclip.%s", g_get_user_runtime_dir(), ct->display
        );
    }
    else
        path = g_strdup(ct->sock_path);

    g_autoptr(GError) error = NULL;
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path);

    g_autoptr(GSocketConnection) sock_ct = g_socket_client_connect(
        client, G_SOCKET_CONNECTABLE(addr), NULL, &error
    );

    if (sock_ct == NULL)
    {
        g_warning("Error connecting to IPC server: %s", error->message);
        return NULL;
    }

    GSocket *sock = g_socket_connection_get_socket(sock_ct);

    struct ipc_ct ict;

    if (!ipc_ct_init(&ict, g_socket_get_fd(sock)))
        return NULL;

    while (TRUE)
    {
        g_mutex_lock(&ct->stop_mut);
        gboolean stop = ct->stop;
        g_mutex_unlock(&ct->stop_mut);

        if (stop)
            break;

        SwayclipRequest *req;

        while ((req = g_async_queue_try_pop(ct->request_queue)) != NULL)
        {
            ipc_ct_write_msg(&ict, IPC_MESSAGE_CALL, req->obj, req->scm_fd);
            g_free(req);
        }

        GIOCondition cond = G_IO_IN;

        if (ipc_ct_has_pending_writes(&ict) &&
            g_socket_condition_check(sock, G_IO_OUT))
        {
            while (ipc_ct_has_pending_writes(&ict))
            {
                bool need_poll = false;

                if (!ipc_ct_write(&ict, &need_poll))
                {
                    if (need_poll)
                        cond |= G_IO_OUT;
                    else
                        cond = G_IO_ERR;
                    break;
                }
            }

            if (cond == G_IO_ERR)
                break;
        }
        else if (ipc_ct_has_pending_writes(&ict))
            cond |= G_IO_OUT;

        if (!g_socket_condition_wait(sock, cond, ct->cancel_poll, &error))
        {
            if (error->code == G_IO_ERROR_CANCELLED)
            {
                g_clear_error(&error);
                g_cancellable_reset(ct->cancel_poll);
            }
            else
            {
                g_warning("Error polling IPC server: %s", error->message);
                break;
            }
        }

        if (g_socket_condition_check(sock, G_IO_IN))
        {
            // Must take ownership because we transfer ownership of the struct
            // json_object to another thread in the callback.
            if (!ipc_ct_read(
                    &ict,
                    IPC_CT_NO_MMAP | IPC_CT_TAKE_OWNERSHIP | IPC_CT_WANT_SCM_FD,
                    ipc_message_callback,
                    ct
                ))
                break;
        }
    }

    ict.fd = -1;
    ipc_ct_uninit(&ict);

    return NULL;
}

static void
swayclip_request_free(SwayclipRequest *req)
{
    json_object_put(req->obj);
    if (req->scm_fd != -1)
        close(req->scm_fd);
    g_free(req);
}

static void
swayclip_connection_init(SwayclipConnection *self)
{
    self->ipc_thread =
        g_thread_new("IPC thread", (GThreadFunc)ipc_thread_cb, self);

    self->cancel_poll = g_cancellable_new();

    self->request_queue =
        g_async_queue_new_full((GDestroyNotify)swayclip_request_free);
    self->pending_queue =
        g_async_queue_new_full((GDestroyNotify)swayclip_message_free);
    self->task_queue = g_queue_new();

    g_mutex_init(&self->stop_mut);
    g_mutex_init(&self->idle_mut);
}

SwayclipConnection *
swayclip_connection_new(void)
{
    SwayclipConnection *ct = g_object_new(SWAYCLIP_TYPE_CONNECTION, NULL);

    const char *display = g_getenv("WAYLAND_DISPLAY");
    const char *sock = g_getenv("SWAYCLIP_SOCK");

    if (display != NULL)
    {
        const char *suffix = strrchr(display, '/');

        if (suffix == NULL)
            ct->display = g_strdup(display);
        else
            ct->display = g_strdup(suffix);
    }

    if (sock != NULL)
        ct->sock_path = g_strdup(sock);

    return ct;
}

/*
 * Send a request to the daemon. "req" is taken full ownership of, and should
 * only have one reference to it.
 */
void
swayclip_connection_request(
    SwayclipConnection *self,
    struct json_object *obj,
    int                 scm_fd, // May be -1
    int                 io_priority,
    GCancellable       *cancellable,
    GAsyncReadyCallback callback,
    void               *udata
)
{
    g_assert(SWAYCLIP_IS_CONNECTION(self));
    g_assert(obj != NULL);
    g_assert(scm_fd == -1 || scm_fd > 0);
    g_assert(cancellable == NULL || G_IS_CANCELLABLE(cancellable));

    GTask *task = g_task_new(self, cancellable, callback, udata);

    g_task_set_source_tag(task, swayclip_connection_request);
    g_task_set_priority(task, io_priority);

    g_queue_push_tail(self->task_queue, task);

    SwayclipRequest *req = g_new0(SwayclipRequest, 1);

    req->obj = obj;
    req->scm_fd = scm_fd;

    g_async_queue_push(self->request_queue, req);
    g_cancellable_cancel(self->cancel_poll);
}

SwayclipMessage *
swayclip_connection_request_finish(
    SwayclipConnection *self, GAsyncResult *result, GError **error
)
{
    g_assert(SWAYCLIP_IS_CONNECTION(self));
    g_assert(G_IS_ASYNC_RESULT(result));
    g_assert(g_task_is_valid(result, self));
    g_assert(error == NULL || *error == NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

SwayclipMessage *
swayclip_message_copy(SwayclipMessage *msg)
{
    g_assert(msg != NULL);

    SwayclipMessage *new = g_new0(SwayclipMessage, 1);

    msg->obj = json_object_get(msg->obj);
    if (msg->aux_data != NULL)
        new->aux_data = g_mapped_file_ref(msg->aux_data);
    return new;
}

void
swayclip_message_free(SwayclipMessage *msg)
{
    g_assert(msg != NULL);

    json_object_put(msg->obj);
    if (msg->aux_data != NULL)
        g_mapped_file_unref(msg->aux_data);
    g_free(msg);
}
