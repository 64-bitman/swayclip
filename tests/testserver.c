#include "common/util.h"
#include "xarray.h"
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <json.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

xarray_create(char, char, uint32_t, 128, 2);

static struct wl_display  *display;
static struct wlr_backend *backend;

static struct xarray_seat seats;
xarray_create(struct wlr_seat *, seat, uint32_t, 2, 1.5);

static struct wlr_ext_data_control_manager_v1 *ext_data_mgr;

static int
signal_handler(int signo UNUSED, void *udata UNUSED)
{
    wl_display_terminate(display);
    return 0;
}

struct mime_type
{
    char    *name;
    char    *data;
    uint32_t sz;
};
xarray_create(struct mime_type, mime_type, uint32_t, 2, 1.5);

struct data_source
{
    union
    {
        struct wlr_data_source              regular;
        struct wlr_primary_selection_source primary;
    } base;
    struct xarray_mime_type mime_types;
};

static void
data_source_impl_send(
    struct wlr_data_source *wlr_source, const char *mime_type, int fd
)
{
    struct data_source *source = (struct data_source *)wlr_source;
    struct mime_type   *mt = NULL;

    xarray_foreach(mime_type, &source->mime_types, mt)
    {
        if (strcmp(mt->name, mime_type) == 0)
            break;
    }

    if (mt == NULL)
    {
        close(fd);
        return;
    }

    uint32_t remaining = mt->sz;

    while (remaining > 0)
    {
        uint32_t off = mt->sz - remaining;

        ssize_t w = write(fd, mt->data + off, remaining);

        if (w == -1)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        remaining -= w;
    }

    close(fd);
}

static void
data_source_impl_destroy(struct wlr_data_source *wlr_source)
{
    struct data_source *source = (struct data_source *)wlr_source;
    struct mime_type   *mt;

    xarray_foreach(mime_type, &source->mime_types, mt)
    {
        free(mt->name);
        free(mt->data);
    }
}

static const struct wlr_data_source_impl data_source_impl = {
    .send = data_source_impl_send, .destroy = data_source_impl_destroy
};
static const struct wlr_primary_selection_source_impl primary_source_impl = {
    .send = (void (*)())data_source_impl_send,
    .destroy = (void (*)())data_source_impl_destroy
};

static bool
execute_command(struct json_object *obj)
{
    const char *cmd;

    cmd = json_object_get_string(json_object_object_get(obj, "cmd"));

    // Create a new seat
    if (strcmp(cmd, "add_seat") == 0)
    {
        const char *name =
            json_object_get_string(json_object_object_get(obj, "name"));

        struct wlr_seat *seat = wlr_seat_create(display, name);

        xarray_add_seat(&seats, seat);
    }
    else if (strcmp(cmd, "set") == 0 || strcmp(cmd, "get") == 0)
    {
        const char *seat_name =
            json_object_get_string(json_object_object_get(obj, "seat"));
        const char *sel =
            json_object_get_string(json_object_object_get(obj, "sel"));
        bool regular = strcmp(sel, "regular") == 0;

        struct wlr_seat *seat = NULL;

        xarray_foreach_val(seat, &seats, seat)
        {
            if (strcmp(seat->name, seat_name) == 0)
                break;
        }

        if (seat == NULL)
            return false;

        // Set clipboard for given seat and selection
        if (strcmp(cmd, "set") == 0)
        {
            struct json_object *mime_types =
                json_object_object_get(obj, "mime_types");

            struct data_source *source = calloc(1, sizeof(*source));

            assert(source != NULL);
            if (regular)
                wlr_data_source_init(&source->base.regular, &data_source_impl);
            else
                wlr_primary_selection_source_init(
                    &source->base.primary, &primary_source_impl
                );

            for (size_t i = 0; i < json_object_array_length(mime_types); i++)
            {
                const char *mime_type = json_object_get_string(
                    json_object_array_get_idx(mime_types, i)
                );

                char **p;

                if (regular)
                    p = wl_array_add(
                        &source->base.regular.mime_types, sizeof(char *)
                    );
                else
                    p = wl_array_add(
                        &source->base.primary.mime_types, sizeof(char *)
                    );

                assert(p != NULL);
                *p = strdup(mime_type);
                assert(*p != NULL);
            }

            uint32_t serial = wl_display_next_serial(display);

            // Set seat selection
            if (regular)
                wlr_seat_set_selection(
                    seat, (struct wlr_data_source *)source, serial
                );
            else
                wlr_seat_set_primary_selection(
                    seat, (struct wlr_primary_selection_source *)source, serial
                );
        }
        // Get contents of mime type for given seat and selection
        else if (strcmp(cmd, "get") == 0)
        {
        }
    }

    return true;
}

static int
input_handler(int fd, uint32_t mask, void *udata UNUSED)
{
    if (!(mask & WL_EVENT_READABLE))
        return 0;

    struct xarray_char buf;

    xarray_init_char(&buf);

    while (true)
    {
        char    c;
        ssize_t r = read(fd, &c, 1);

        if (r == -1)
        {
            if (errno == EINTR)
                continue;
            wlr_log_errno(WLR_ERROR, "Error reading stdin");
            wl_display_terminate(display);
            return 0;
        }
        else if (r == 0)
        {
            wlr_log(WLR_ERROR, "stdin closed");
            wl_display_terminate(display);
            return 0;
        }

        if (c == '\n')
        {
            xarray_add_char(&buf, NUL);
            break;
        }

        xarray_add_char(&buf, c);
    }

    struct json_object *obj = json_tokener_parse(xarray_data_char(&buf));

    xarray_uninit_char(&buf);
    if (obj != NULL)
    {
        if (!execute_command(obj))
            wlr_log(WLR_INFO, "Invalid JSON input");
        json_object_put(obj);
    }
    else
        wlr_log(WLR_INFO, "JSON parsing error");

    return 0;
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"display", required_argument, 0, 'd'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    char *display_name = NULL;

    while ((c = getopt_long(argc, argv, "d:", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'd':
            display_name = strdup(optarg);
            break;
        default:
            return EXIT_FAILURE;
        }
    }

    wlr_log_init(WLR_DEBUG, NULL);

    struct wl_event_loop *loop;

    display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    backend = wlr_headless_backend_create(loop);
    assert(backend != NULL);

    ext_data_mgr = wlr_ext_data_control_manager_v1_create(display, 1);

    assert(wl_display_add_socket(display, display_name) != -1);
    free(display_name);

    assert(wlr_backend_start(backend));

    struct wl_event_source *sources[3];

    sources[0] = wl_event_loop_add_signal(loop, SIGTERM, signal_handler, NULL);
    sources[1] = wl_event_loop_add_signal(loop, SIGINT, signal_handler, NULL);

    sources[2] = wl_event_loop_add_fd(
        loop, STDIN_FILENO, WL_EVENT_READABLE, input_handler, NULL
    );

    xarray_init_seat(&seats);

    wl_display_run(display);

    for (int i = 0; i < N_ELEMENTS(sources); i++)
        wl_event_source_remove(sources[i]);

    wl_display_destroy_clients(display);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);

    return EXIT_SUCCESS;
}
