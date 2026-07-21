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

#include "client.h"
#include "protocols/ext-data-control-v1.h"
#include "protocols/ext-transient-seat-v1.h"
#include "util.h"
#include <gio-unix-2.0/gio/gunixinputstream.h>
#include <gio-unix-2.0/gio/gunixoutputstream.h>
#include <gio/gio.h>
#include <glib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <wayland-client.h>

typedef struct Client
{
    char *display;

    int event_fd; // Used to wakeup thread

    gboolean ready;
    GMutex   ready_mut;
    GCond    ready_cond;

    // Only set by worker thread. Should not be accessed if "ready" is FALSE.
    char *seat_name;

    gboolean stop;
    GMutex   stop_mut;

    // Each item in queue is a hash table that maps mime type to GBytes.
    // Ownership of table is taken by receiver.
    GAsyncQueue *copy_regular;
    GAsyncQueue *copy_primary;

    GAsyncQueue *paste_regular;
    GAsyncQueue *paste_primary;

    gboolean regular_cleared;
    gboolean primary_cleared;
    GCond    cleared_cond;
    GMutex   cleared_mut;

    GThread *thread;
} Client;

typedef struct State State;

typedef struct
{
    State *state;

    struct ext_data_control_source_v1 *ext_data_source;

    // Maps mime type to GBytes
    GHashTable *source_data;
} SelState;

typedef struct State
{
    Client             *client;
    struct wl_display  *display;
    struct wl_registry *registry;

    struct wl_seat *seat;
    uint32_t        seat_global_name;

    struct ext_transient_seat_manager_v1 *tseat_mgr;
    struct ext_transient_seat_v1         *tseat;

    struct ext_data_control_manager_v1 *ext_data_mgr;
    struct ext_data_control_device_v1  *ext_data_device;

    SelState regular;
    SelState primary;

    // Used to hold current data offer mime types
    GPtrArray *mime_types;
} State;

static void
client_wakeup(Client *client)
{
    uint64_t i = 1;
    write(client->event_fd, &i, sizeof(i));
}

static void
client_stop(Client *client)
{
    g_mutex_lock(&client->stop_mut);
    client->stop = TRUE;
    g_mutex_unlock(&client->stop_mut);
    client_wakeup(client);
}

static void
event_noop()
{
}

static void
seat_event_name(void *udata, struct wl_seat *proxy UNUSED, const char *name)
{
    State *state = udata;

    state->client->seat_name = g_strdup(name);
}

static const struct wl_seat_listener seat_listener = {
    .name = seat_event_name, .capabilities = event_noop
};

static void
tseat_event_ready(
    void *udata, struct ext_transient_seat_v1 *proxy UNUSED, uint32_t name
)
{
    State *state = udata;

    state->seat = wl_registry_bind(
        state->registry, name, &wl_seat_interface, WL_SEAT_NAME_SINCE_VERSION
    );
    state->seat_global_name = name;

    wl_seat_add_listener(state->seat, &seat_listener, state);
}

static void
tseat_event_denied(
    void *udata UNUSED, struct ext_transient_seat_v1 *proxy UNUSED
)
{
    g_assert_not_reached();
}

static const struct ext_transient_seat_v1_listener tseat_listener = {
    .ready = tseat_event_ready, .denied = tseat_event_denied
};

static void
registry_event_global(
    void               *udata,
    struct wl_registry *proxy,
    uint32_t            name,
    const char         *interface,
    uint32_t            version
)
{
    State *state = udata;

    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
    {
        state->ext_data_mgr = wl_registry_bind(
            proxy, name, &ext_data_control_manager_v1_interface, 1
        );
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        g_assert_cmpint(version, >=, WL_SEAT_NAME_SINCE_VERSION);
    }
    else if (
        strcmp(interface, ext_transient_seat_manager_v1_interface.name) == 0
    )
    {
        state->tseat_mgr = wl_registry_bind(
            proxy, name, &ext_transient_seat_manager_v1_interface, 1
        );

        state->tseat = ext_transient_seat_manager_v1_create(state->tseat_mgr);

        ext_transient_seat_v1_add_listener(
            state->tseat, &tseat_listener, state
        );
    }
}

static void
registry_event_global_remove(
    void *udata, struct wl_registry *proxy UNUSED, uint32_t name
)
{
    State *state = udata;

    // Stop client if seat was removed.
    if (state->seat_global_name == name)
    {
        wl_seat_destroy(state->seat);
        state->seat = NULL;
        client_stop(state->client);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_global,
    .global_remove = registry_event_global_remove
};

static void
sel_state_init(SelState *sel, State *state)
{
    sel->state = state;
}

static void
sel_state_uninit(SelState *sel)
{
    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);

    if (sel->source_data != NULL)
        g_hash_table_unref(sel->source_data);
}

static void
data_source_event_send(
    void                                    *udata,
    struct ext_data_control_source_v1 *proxy UNUSED,
    const char                              *mime_type,
    int                                      fd
)
{
    SelState *sel = udata;
    GBytes   *bytes = g_hash_table_lookup(sel->source_data, mime_type);

    if (bytes == NULL)
    {
        close(fd);
        return;
    }

    g_autoptr(GOutputStream) stream = g_unix_output_stream_new(fd, FALSE);

    size_t         sz;
    const uint8_t *data = g_bytes_get_data(bytes, &sz);

    ASSERT_NOERROR(
        g_output_stream_write_all(stream, data, sz, NULL, NULL, &ERROR)
    );
    close(fd);
}

static void
data_source_event_cancelled(
    void *udata, struct ext_data_control_source_v1 *proxy
)
{
    SelState *sel = udata;

    if (sel->ext_data_source == proxy)
    {
        g_hash_table_unref(sel->source_data);
        sel->source_data = NULL;
        sel->ext_data_source = NULL;
    }
    ext_data_control_source_v1_destroy(proxy);
}

static const struct ext_data_control_source_v1_listener data_source_listener = {
    .send = data_source_event_send, .cancelled = data_source_event_cancelled
};

static gboolean
sel_state_copy(SelState *sel, GAsyncQueue *queue)
{
    GHashTable *mime_types = g_async_queue_try_pop(queue);

    if (mime_types == NULL)
        return FALSE;

    if (sel->source_data != NULL)
        g_hash_table_unref(sel->source_data);
    sel->source_data = mime_types;

    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);
    sel->ext_data_source = ext_data_control_manager_v1_create_data_source(
        sel->state->ext_data_mgr
    );

    ext_data_control_source_v1_add_listener(
        sel->ext_data_source, &data_source_listener, sel
    );

    GHashTableIter iter;
    const char    *mime_type;

    g_hash_table_iter_init(&iter, mime_types);
    while (g_hash_table_iter_next(&iter, (void **)&mime_type, NULL))
        ext_data_control_source_v1_offer(sel->ext_data_source, mime_type);

    if (sel == &sel->state->regular)
        ext_data_control_device_v1_set_selection(
            sel->state->ext_data_device, sel->ext_data_source
        );
    else
        ext_data_control_device_v1_set_primary_selection(
            sel->state->ext_data_device, sel->ext_data_source
        );

    return TRUE;
}

static void
data_offer_event_offer(
    void                                   *udata,
    struct ext_data_control_offer_v1 *proxy UNUSED,
    const char                             *mime_type
)
{
    GPtrArray *arr = udata;

    g_ptr_array_add(arr, g_strdup(mime_type));
}

static const struct ext_data_control_offer_v1_listener data_offer_listener = {
    .offer = data_offer_event_offer
};

static void
data_device_event_data_offer(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    State *state = udata;

    g_assert_null(state->mime_types);
    state->mime_types = g_ptr_array_new_with_free_func(g_free);

    ext_data_control_offer_v1_add_listener(
        offer_proxy, &data_offer_listener, state->mime_types
    );
}

static void
sel_state_selection(SelState *sel, struct ext_data_control_offer_v1 *offer)
{
    g_autoptr(GPtrArray) mime_types = sel->state->mime_types;

    if (offer == NULL)
    {
        g_mutex_lock(&sel->state->client->cleared_mut);
        if (sel == &sel->state->regular)
            sel->state->client->regular_cleared = TRUE;
        else
            sel->state->client->primary_cleared = TRUE;
        g_cond_signal(&sel->state->client->cleared_cond);
        g_mutex_unlock(&sel->state->client->cleared_mut);
        return;
    }

    sel->state->mime_types = NULL;

    if (sel->ext_data_source != NULL)
    {
        ext_data_control_offer_v1_destroy(offer);
        return;
    }

    GHashTable *table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
    );

    uint8_t buf[4096];

    while (mime_types->len > 0)
    {
        char *mime_type = mime_types->pdata[0];

        g_ptr_array_steal_index_fast(mime_types, 0);

        int fds[2];

        g_assert_no_errno(pipe(fds));
        ext_data_control_offer_v1_receive(offer, mime_type, fds[1]);
        close(fds[1]);
        wl_display_flush(sel->state->display);

        g_autoptr(GInputStream) stream = g_unix_input_stream_new(fds[0], TRUE);
        GByteArray *arr = g_byte_array_new();

        while (TRUE)
        {
            ssize_t r;

            ASSERT_NOERROR(
                r = g_input_stream_read(stream, buf, sizeof(buf), NULL, &ERROR)
            );

            if (r == 0)
                break;

            g_byte_array_append(arr, buf, r);
        }

        GBytes *bytes = g_byte_array_free_to_bytes(arr);

        g_hash_table_insert(table, mime_type, bytes);
    }

    if (sel == &sel->state->regular)
        g_async_queue_push(sel->state->client->paste_regular, table);
    else
        g_async_queue_push(sel->state->client->paste_primary, table);

    ext_data_control_offer_v1_destroy(offer);
}

static void
data_device_event_selection(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    State *state = udata;

    sel_state_selection(&state->regular, offer_proxy);
}

static void
data_device_event_primary_selection(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    State *state = udata;

    sel_state_selection(&state->primary, offer_proxy);
}

static void
data_device_event_finished(
    void *udata UNUSED, struct ext_data_control_device_v1 *proxy UNUSED
)
{
    g_assert_not_reached();
}

static const struct ext_data_control_device_v1_listener data_device_listener = {
    .data_offer = data_device_event_data_offer,
    .selection = data_device_event_selection,
    .primary_selection = data_device_event_primary_selection,
    .finished = data_device_event_finished
};

static void *
client_thread(Client *client)
{
    State state = {0};

    state.client = client;

    state.display = wl_display_connect(client->display);
    g_assert_nonnull(state.display);

    state.registry = wl_display_get_registry(state.display);

    wl_registry_add_listener(state.registry, &registry_listener, &state);

    wl_display_roundtrip(state.display);

    g_assert_nonnull(state.ext_data_mgr);
    g_assert_nonnull(state.tseat_mgr);

    // Roundtrip again to get "ready" event for transient seat.
    wl_display_roundtrip(state.display);
    g_assert_nonnull(state.seat);

    // Again to get wl_seat name
    wl_display_roundtrip(state.display);
    g_assert_nonnull(state.client->seat_name);

    state.ext_data_device = ext_data_control_manager_v1_get_data_device(
        state.ext_data_mgr, state.seat
    );

    ext_data_control_device_v1_add_listener(
        state.ext_data_device, &data_device_listener, &state
    );

    sel_state_init(&state.regular, &state);
    sel_state_init(&state.primary, &state);

    g_mutex_lock(&client->ready_mut);
    client->ready = TRUE;
    g_cond_signal(&client->ready_cond);
    g_mutex_unlock(&client->ready_mut);

    struct pollfd pfds[2];

    pfds[0] = (struct pollfd){
        .fd = wl_display_get_fd(state.display), .events = POLLIN
    };
    pfds[1] = (struct pollfd){.fd = client->event_fd, .events = POLLIN};

    while (TRUE)
    {
        wl_display_flush(state.display);

        int ret = poll(pfds, 2, -1);

        g_assert_no_errno(ret);

        if (pfds[1].revents & POLLIN)
        {
            uint64_t val;
            ssize_t  r = read(client->event_fd, &val, sizeof(val));

            g_assert_no_errno(r);
        }

        g_mutex_lock(&client->stop_mut);
        gboolean stop = client->stop;
        g_mutex_unlock(&client->stop_mut);

        if (stop)
            break;

        if (pfds[0].revents & POLLIN)
            wl_display_dispatch(state.display);

        sel_state_copy(&state.regular, client->copy_regular);
        sel_state_copy(&state.primary, client->copy_primary);
    }

    sel_state_uninit(&state.regular);
    sel_state_uninit(&state.primary);

    if (state.ext_data_device != NULL)
        ext_data_control_device_v1_destroy(state.ext_data_device);
    if (state.ext_data_mgr != NULL)
        ext_data_control_manager_v1_destroy(state.ext_data_mgr);

    if (state.tseat_mgr != NULL)
        ext_transient_seat_manager_v1_destroy(state.tseat_mgr);
    if (state.tseat != NULL)
        ext_transient_seat_v1_destroy(state.tseat);

    if (state.seat != NULL)
        wl_seat_destroy(state.seat);

    wl_registry_destroy(state.registry);
    wl_display_disconnect(state.display);

    return NULL;
}

Client *
client_new(const char *display)
{
    Client *client = g_new0(Client, 1);

    client->display = g_strdup(display);

    client->ready = FALSE;
    g_mutex_init(&client->ready_mut);
    g_cond_init(&client->ready_cond);

    client->seat_name = NULL;

    client->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    g_assert_no_errno(client->event_fd);

    client->stop = FALSE;
    g_mutex_init(&client->stop_mut);

    g_mutex_init(&client->cleared_mut);
    g_cond_init(&client->cleared_cond);

    client->copy_regular =
        g_async_queue_new_full((GDestroyNotify)g_hash_table_unref);
    client->copy_primary =
        g_async_queue_new_full((GDestroyNotify)g_hash_table_unref);

    client->paste_regular =
        g_async_queue_new_full((GDestroyNotify)g_hash_table_unref);
    client->paste_primary =
        g_async_queue_new_full((GDestroyNotify)g_hash_table_unref);

    static int i;

    g_autofree char *name = g_strdup_printf("Test client %d", i++);
    client->thread = g_thread_new(name, (GThreadFunc)client_thread, client);

    // Wait for thread to become ready
    g_mutex_lock(&client->ready_mut);
    while (!client->ready)
        g_cond_wait(&client->ready_cond, &client->ready_mut);
    g_mutex_unlock(&client->ready_mut);

    return client;
}

void
client_free(Client *client)
{
    client_stop(client);
    g_thread_join(client->thread);

    close(client->event_fd);
    g_mutex_clear(&client->ready_mut);
    g_cond_clear(&client->ready_cond);
    g_mutex_clear(&client->stop_mut);
    g_cond_clear(&client->cleared_cond);
    g_mutex_clear(&client->cleared_mut);

    g_async_queue_unref(client->copy_regular);
    g_async_queue_unref(client->copy_primary);
    g_async_queue_unref(client->paste_regular);
    g_async_queue_unref(client->paste_primary);

    g_free(client->seat_name);
    g_free(client->display);
    g_free(client);
}

const char *
client_get_seat(Client *client)
{
    g_assert_true(client->ready);
    return client->seat_name;
}

/*
 * Queue data to be copied. Variadic args are in format of <mime type>, <data>,
 * <size>. Terminate with <mime type> being NULL.
 */
void
client_copy(Client *client, SelectionType sel, ...)
{
    va_list ap;

    GHashTable *table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
    );

    va_start(ap, sel);

    while (TRUE)
    {
        const char *mime_type = va_arg(ap, const char *);

        if (mime_type == NULL)
            break;

        const uint8_t *data = va_arg(ap, const uint8_t *);
        ssize_t        sz = va_arg(ap, ssize_t);

        GBytes *bytes = g_bytes_new(data, sz);

        g_hash_table_insert(table, g_strdup(mime_type), bytes);
    }
    va_end(ap);

    if (sel == SELECTION_REGULAR)
        g_async_queue_push(client->copy_regular, table);
    else
        g_async_queue_push(client->copy_primary, table);
    client_wakeup(client);
}

GHashTable *
client_paste(Client *client, SelectionType sel)
{
    GHashTable *table;

    if (sel == SELECTION_REGULAR)
        table = g_async_queue_pop(client->paste_regular);
    else
        table = g_async_queue_pop(client->paste_primary);

    return table;
}

/*
 * Note that this uses static memory!
 */
const char *
client_paste_mime(
    Client *client, SelectionType sel, const char *mime_type, size_t *len
)
{
    g_autoptr(GHashTable) table = client_paste(client, sel);

    GBytes *bytes;
    void   *ptr;

    if (!g_hash_table_steal_extended(table, mime_type, &ptr, (void **)&bytes))
        return NULL;

    g_free(ptr);

    static char *str;
    size_t       sz;

    g_free(str);
    str = g_bytes_unref_to_data(bytes, &sz);
    if (len != NULL)
        *len = sz;
    return str;
}
