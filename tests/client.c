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

#include "common/util.h"
#include "protocols/ext-data-control-v1.h"
#include "protocols/ext-transient-seat-v1.h"
#include "xarray.h"
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <json.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>

#define printerr(fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#define printerrno(fmt, ...)                                                   \
    fprintf(stderr, fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

xarray_create(char *, str, uint32_t, 10, 1.5);
xarray_create(char, char, uint32_t, 128, 1.5);

struct mime_type
{
    char    *name;
    char    *data;
    uint32_t sz;
};
xarray_create(struct mime_type, mime_type, uint32_t, 10, 1.5);

struct selection
{
    bool dirty; // Set when a new offer/selection event has been processed

    struct ext_data_control_source_v1 *ext_data_source;

    struct xarray_mime_type mime_types;
    // Used to differentiate between a selection with no mime types and a nil
    // selection event.
    bool cleared;
};

static struct wl_display  *display;
static struct wl_registry *registry;

static char *wanted_seat; // Seat to use instead of creating a
                          // transient seat.
static struct wl_seat *seat;
static char           *seat_name;

static struct ext_transient_seat_manager_v1 *tseat_mgr;
static struct ext_transient_seat_v1         *tseat;

static struct ext_data_control_manager_v1 *ext_data_mgr;
static struct ext_data_control_device_v1  *ext_data_device;

static struct selection regular;
static struct selection primary;

// Used to hold current data offer mime types
static struct xarray_str mime_types;

static void
wayland_event_noop()
{
}

static void
seat_event_name(void *udata UNUSED, struct wl_seat *proxy, const char *name)
{
    if (seat != NULL || wanted_seat == NULL || strcmp(name, wanted_seat) == 0)
    {
        seat_name = strdup(name);
        seat = proxy;
    }
    else
        wl_seat_destroy(proxy);
}

static const struct wl_seat_listener seat_listener = {
    .name = seat_event_name, .capabilities = wayland_event_noop
};

static void
registry_event_global(
    void *udata               UNUSED,
    struct wl_registry *proxy UNUSED,
    uint32_t                  name,
    const char               *interface,
    uint32_t version          UNUSED
)
{
    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
    {
        ext_data_mgr = wl_registry_bind(
            registry, name, &ext_data_control_manager_v1_interface, 1
        );
    }
    else if (
        strcmp(interface, ext_transient_seat_manager_v1_interface.name) == 0
    )
    {
        tseat_mgr = wl_registry_bind(
            registry, name, &ext_transient_seat_manager_v1_interface, 1
        );
    }
    else if (
        wanted_seat != NULL && strcmp(interface, wl_seat_interface.name) == 0
    )
    {
        struct wl_seat *seat_proxy = wl_registry_bind(
            registry, name, &wl_seat_interface, WL_SEAT_NAME_SINCE_VERSION
        );

        wl_seat_add_listener(seat_proxy, &seat_listener, NULL);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_global, .global_remove = wayland_event_noop
};

static void
tseat_event_ready(
    void *udata                         UNUSED,
    struct ext_transient_seat_v1 *proxy UNUSED,
    uint32_t                            name
)
{
    seat = wl_registry_bind(
        registry, name, &wl_seat_interface, WL_SEAT_NAME_SINCE_VERSION
    );
}

static void
tseat_event_denied(
    void *udata UNUSED, struct ext_transient_seat_v1 *proxy UNUSED
)
{
    printerr("Transient seat denied!");
    exit(1);
}

static const struct ext_transient_seat_v1_listener tseat_listener = {
    .ready = tseat_event_ready, .denied = tseat_event_denied
};

static void
data_offer_event_offer(
    void *udata                             UNUSED,
    struct ext_data_control_offer_v1 *offer UNUSED,
    const char                             *mime_type
)
{
    char *str = strdup(mime_type);

    assert(str != NULL);
    xarray_add_str(&mime_types, str);
}

static const struct ext_data_control_offer_v1_listener data_offer_listener = {
    .offer = data_offer_event_offer
};

static void
clear_mime_types(void)
{
    for (uint32_t i = 0; i < xarray_len_str(&mime_types); i++)
        free(xarray_val_str(&mime_types, i));
    xarray_clear_str(&mime_types);
}

static void
sel_clear_mime_types(struct selection *sel)
{
    for (uint32_t i = 0; i < xarray_len_mime_type(&sel->mime_types); i++)
    {
        free(xarray_val_mime_type(&sel->mime_types, i).name);
        free(xarray_val_mime_type(&sel->mime_types, i).data);
    }
    xarray_clear_mime_type(&sel->mime_types);
}

static void
data_device_event_data_offer(
    void *udata                              UNUSED,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer
)
{
    clear_mime_types();
    ext_data_control_offer_v1_add_listener(offer, &data_offer_listener, NULL);
}

static void
handle_selection(struct selection *sel, struct ext_data_control_offer_v1 *offer)
{
    sel->dirty = true;

    if (sel->ext_data_source != NULL)
    {
        if (offer != NULL)
            ext_data_control_offer_v1_destroy(offer);
        clear_mime_types();
        return;
    }

    if (offer == NULL)
    {
        sel->cleared = true;
        sel_clear_mime_types(sel);
        clear_mime_types();
        return;
    }

    sel->cleared = false;

    sel_clear_mime_types(sel);

    while (xarray_len_str(&mime_types) > 0)
    {
        char *mime_type = xarray_val_str(&mime_types, 0);

        xarray_del_str(&mime_types, 0);

        int fds[2];

        assert(pipe(fds) != -1);
        ext_data_control_offer_v1_receive(offer, mime_type, fds[1]);
        close(fds[1]);
        wl_display_flush(display);

        struct mime_type   mt;
        struct xarray_char arr;
        static char        buf[128];

        xarray_init_char(&arr);

        while (true)
        {
            ssize_t r = read(fds[0], buf, sizeof(buf));

            if (r == -1)
            {
                if (errno == EINTR)
                    continue;
                printerrno("read() returned -1 while receiving data");
                exit(1);
            }
            else if (r == 0)
                break;

            xarray_concat_char(&arr, buf, r);
        }

        mt.name = mime_type;
        mt.data = xarray_steal_char(&arr, &mt.sz);

        xarray_add_mime_type(&sel->mime_types, mt);
    }

    ext_data_control_offer_v1_destroy(offer);
}

static void
data_device_event_selection(
    void *udata                              UNUSED,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer
)
{
    handle_selection(&regular, offer);
}

static void
data_device_event_primary_selection(
    void *udata                              UNUSED,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer
)
{
    handle_selection(&primary, offer);
}

static void
data_device_event_finished(
    void *udata UNUSED, struct ext_data_control_device_v1 *proxy UNUSED
)
{
    printerr("Data device finished");
    exit(1);
}

static const struct ext_data_control_device_v1_listener data_device_listener = {
    .data_offer = data_device_event_data_offer,
    .selection = data_device_event_selection,
    .primary_selection = data_device_event_primary_selection,
    .finished = data_device_event_finished,
};

static void
data_source_event_send(
    void                                    *udata,
    struct ext_data_control_source_v1 *proxy UNUSED,
    const char                              *mime_type,
    int                                      fd
)
{
    struct selection *sel = udata;
    struct mime_type *mt = NULL;

    xarray_foreach(mime_type, &sel->mime_types, mt)
    {
        if (strcmp(mt->name, mime_type) == 0)
            goto found;
    }
found:

    if (mt != NULL)
    {
        char    *ptr = mt->data;
        uint32_t remaining = mt->sz;

        while (true)
        {
            ssize_t w = write(fd, ptr, remaining);

            if (w == -1)
            {
                if (errno == EINTR)
                    continue;
                printerrno("write() returned -1 while writing data");
                exit(1);
            }
            else if (w == 0)
                break;

            ptr += w;
            remaining -= w;
        }
    }

    close(fd);
}

static void
data_source_event_cancelled(
    void *udata, struct ext_data_control_source_v1 *proxy
)
{
    struct selection *sel = udata;

    if (sel->ext_data_source == proxy)
    {
        sel_clear_mime_types(sel);
        sel->ext_data_source = NULL;
    }
    ext_data_control_source_v1_destroy(proxy);
}

static const struct ext_data_control_source_v1_listener data_source_listener = {
    .send = data_source_event_send, .cancelled = data_source_event_cancelled
};

static void
sel_poll(struct selection *sel)
{
    sel->dirty = false;

    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};

    while (!sel->dirty)
    {
        wl_display_flush(display);

        int ret = poll(&pfd, 1, 1000);

        if (ret == -1)
        {
            if (errno == EINTR)
                continue;
            printerrno("poll() failed while polling selection");
            exit(1);
        }
        else if (ret == 0)
        {
            printerr("Polling selection timed out");
            break;
        }

        if (pfd.revents & POLLIN)
            wl_display_dispatch(display);
    }
}

static bool
handle_input(struct json_object *obj)
{
    struct json_object *j_action;
    const char         *action;

    if (!json_object_object_get_ex(obj, "action", &j_action) ||
        !json_object_is_type(j_action, json_type_string))
        return false;
    action = json_object_get_string(j_action);

    struct json_object *j_sel;
    const char         *selname;

    if (!json_object_object_get_ex(obj, "sel", &j_sel) ||
        !json_object_is_type(j_sel, json_type_string))
        return false;
    selname = json_object_get_string(j_sel);

    struct selection *sel =
        strcmp(selname, "regular") == 0 ? &regular : &primary;
    struct ext_data_control_source_v1 *source = NULL;

    if (strcmp(action, "get") == 0)
    {
        struct json_object *j_expect;

        // If "expect" is true (default to true if not provided), then a
        // selection event (that is not from us) is expected for this selection.
        // Poll until we get one.
        if (!json_object_object_get_ex(obj, "expect", &j_expect) ||
            (json_object_is_type(j_expect, json_type_boolean) &&
             json_object_get_boolean(j_expect)))
        {
            sel_poll(sel);
        }

        if (sel->cleared)
        {
            printf("\"cleared\"\n");
            fflush(stdout);
            return true;
        }

        struct json_object *resp = json_object_new_object();

        assert(resp != NULL);

        struct mime_type *mt;

        xarray_foreach(mime_type, &sel->mime_types, mt)
        {
            struct json_object *val =
                json_object_new_string_len((char *)mt->data, mt->sz);

            assert(val != NULL);
            json_object_object_add(resp, mt->name, val);
        }

        printf(
            "%s\n", json_object_to_json_string_ext(resp, JSON_C_TO_STRING_PLAIN)
        );
        fflush(stdout);
        json_object_put(resp);

        return true;
    }
    else if (strcmp(action, "clear") == 0)
    {
        goto set;
    }
    else if (strcmp(action, "set") != 0)
        return false;

    struct json_object *j_mime_types;

    if (!json_object_object_get_ex(obj, "mime_types", &j_mime_types) ||
        !json_object_is_type(j_mime_types, json_type_object))
        return false;

    source = ext_data_control_manager_v1_create_data_source(ext_data_mgr);

    sel_clear_mime_types(sel);

    json_object_object_foreach(j_mime_types, mime_type, j_data)
    {
        if (!json_object_is_type(j_data, json_type_string))
            continue;

        struct mime_type mt;

        mt.name = strdup(mime_type);
        assert(mt.name != NULL);
        mt.data = strdup(json_object_get_string(j_data));
        assert(mt.data != NULL);
        mt.sz = json_object_get_string_len(j_data);

        assert(xarray_add_mime_type(&sel->mime_types, mt));

        ext_data_control_source_v1_offer(source, mime_type);
    }

    ext_data_control_source_v1_add_listener(source, &data_source_listener, sel);

set:
    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);
    sel->ext_data_source = source;
    sel->cleared = source == NULL;

    if (sel == &regular)
        ext_data_control_device_v1_set_selection(ext_data_device, source);
    else
        ext_data_control_device_v1_set_primary_selection(
            ext_data_device, source
        );

    return true;
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"seat", required_argument, 0, 's'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "s:", options, &idx)) != -1)
    {
        switch (c)
        {
        case 's':
            wanted_seat = strdup(optarg);
            break;
        default:
            return EXIT_FAILURE;
        }
    }

    display = wl_display_connect(NULL);

    if (display == NULL)
    {
        printerrno("Error connecting to Wayland display");
        return EXIT_FAILURE;
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    // Get initial globals
    wl_display_roundtrip(display);

    assert(ext_data_mgr != NULL);
    assert(tseat_mgr != NULL);

    if (wanted_seat == NULL)
    {
        tseat = ext_transient_seat_manager_v1_create(tseat_mgr);
        ext_transient_seat_v1_add_listener(tseat, &tseat_listener, NULL);
    }

    // Get wl_seat global from transient seat, or if "wanted_seat" is not NULL,
    // then get all seat names and check.
    wl_display_roundtrip(display);

    assert(seat != NULL);
    if (wanted_seat == NULL)
    {
        wl_seat_add_listener(seat, &seat_listener, NULL);

        // Get seat name
        wl_display_roundtrip(display);
    }

    assert(seat_name != NULL);

    struct json_object *obj = json_object_new_string(seat_name);
    printf("%s\n", json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN));
    fflush(stdout);
    json_object_put(obj);

    ext_data_device =
        ext_data_control_manager_v1_get_data_device(ext_data_mgr, seat);
    ext_data_control_device_v1_add_listener(
        ext_data_device, &data_device_listener, NULL
    );

    xarray_init_str(&mime_types);

    xarray_init_mime_type(&regular.mime_types);
    xarray_init_mime_type(&primary.mime_types);

    struct pollfd pfds[2];

    pfds[0] =
        (struct pollfd){.fd = wl_display_get_fd(display), .events = POLLIN};
    pfds[1] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};

    struct xarray_char buf;

    xarray_init_char(&buf);

    while (true)
    {
        wl_display_flush(display);

        int ret = poll(pfds, 2, -1);

        assert(ret != -1);

        if (pfds[1].revents & POLLIN)
        {
            while (true)
            {
                uint8_t c;
                ssize_t r = read(STDIN_FILENO, &c, 1);

                if (r == -1)
                {
                    if (errno == EINTR)
                        continue;
                    printerrno("read() failed for stdin");
                    exit(1);
                }
                else if (r == 0)
                {
                    printerrno("stdin closed?");
                    exit(1);
                }

                if (c == '\n')
                {
                    xarray_add_char(&buf, NUL);
                    break;
                }
                xarray_add_char(&buf, c);
            }

            struct json_object *obj =
                json_tokener_parse((char *)xarray_data_char(&buf));

            xarray_clear_char(&buf);

            if (obj != NULL)
            {
                if (json_object_is_type(obj, json_type_string) &&
                    strcmp(json_object_get_string(obj), "exit") == 0)
                {
                    json_object_put(obj);
                    break;
                }

                if (!handle_input(obj))
                    printerr("Invalid JSON input");
                json_object_put(obj);
            }
            else
                printerr("JSON parsing error");
        }

        if (pfds[0].revents & POLLIN)
            wl_display_dispatch(display);
    }

    return EXIT_SUCCESS;
}
