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

struct seat
{
    struct wlr_seat   *seat;
    struct wl_listener request_set_selection;
    struct wl_listener request_set_primary_selection;
    struct wl_listener destroy;
};
xarray_create(struct seat *, seat, uint32_t, 2, 1.5);

static struct wl_display  *display;
static struct wlr_backend *backend;

static struct xarray_seat seats;

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
            goto next;
    }
next:

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
    xarray_uninit_mime_type(&source->mime_types);
    free(source);
}

static const struct wlr_data_source_impl data_source_impl = {
    .send = data_source_impl_send, .destroy = data_source_impl_destroy
};
static const struct wlr_primary_selection_source_impl primary_source_impl = {
    .send = (void (*)())data_source_impl_send,
    .destroy = (void (*)())data_source_impl_destroy
};

static void
handle_request_set_selection(struct wl_listener *listener, void *data)
{
    struct seat *seat = wl_container_of(listener, seat, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(seat->seat, event->source, event->serial);
}

static void
handle_request_set_primary_selection(struct wl_listener *listener, void *data)
{
    struct seat *seat =
        wl_container_of(listener, seat, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;

    wlr_seat_set_primary_selection(seat->seat, event->source, event->serial);
}
static void
handle_seat_destroy(struct wl_listener *listener, void *data UNUSED)
{
    struct seat *seat = wl_container_of(listener, seat, destroy);

    wl_list_remove(&seat->request_set_selection.link);
    wl_list_remove(&seat->request_set_primary_selection.link);
    wl_list_remove(&seat->destroy.link);
    free(seat);
}

static bool
execute_command(struct json_object *obj)
{
    const char *cmd;

    cmd = json_object_get_string(json_object_object_get(obj, "cmd"));

    fprintf(stderr, "Executing command \"%s\"\n", cmd);

    // Create a new seat
    if (strcmp(cmd, "add_seat") == 0)
    {
        const char *name =
            json_object_get_string(json_object_object_get(obj, "name"));

        struct seat *seat = calloc(1, sizeof(*seat));

        assert(seat != NULL);
        seat->seat = wlr_seat_create(display, name);

        seat->request_set_selection.notify = handle_request_set_selection;
        seat->request_set_primary_selection.notify =
            handle_request_set_primary_selection;
        seat->destroy.notify = handle_seat_destroy;

        wl_signal_add(
            &seat->seat->events.request_set_selection,
            &seat->request_set_selection
        );
        wl_signal_add(
            &seat->seat->events.request_set_primary_selection,
            &seat->request_set_primary_selection
        );
        wl_signal_add(&seat->seat->events.destroy, &seat->destroy);

        xarray_add_seat(&seats, seat);
        printf("\"OK\"\n");
    }
    // Delete seat
    else if (strcmp(cmd, "del_seat") == 0)
    {
        const char *name =
            json_object_get_string(json_object_object_get(obj, "name"));

        struct seat *seat = NULL;

        xarray_foreach_val(seat, &seats, seat)
        {
            if (strcmp(seat->seat->name, name) == 0)
                goto stop;
        }
stop:

        assert(seat != NULL);
        wlr_seat_destroy(seat->seat);
        printf("\"OK\"\n");
    }
    else if (
        strcmp(cmd, "set") == 0 || strcmp(cmd, "get") == 0 ||
        strcmp(cmd, "clear") == 0
    )
    {
        const char *seat_name =
            json_object_get_string(json_object_object_get(obj, "seat"));
        const char *sel =
            json_object_get_string(json_object_object_get(obj, "sel"));
        bool regular = strcmp(sel, "regular") == 0;

        struct seat *seat = NULL;

        xarray_foreach_val(seat, &seats, seat)
        {
            if (strcmp(seat->seat->name, seat_name) == 0)
                goto stop2;
        }
stop2:

        if (seat == NULL)
            return false;

        // Set or clear clipboard for given seat and selection
        if (strcmp(cmd, "set") == 0)
        {
            fprintf(stderr, "Setting clipboard\n");

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

            xarray_init_mime_type(&source->mime_types);

            json_object_object_foreach(mime_types, mime_type, val)
            {
                const char *data = json_object_get_string(val);

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

                struct mime_type mt = {
                    .name = strdup(mime_type),
                };

                // Special case, if "data" is "massive", then create a 2 MB
                // blob of 'a'.
                if (strcmp(data, "massive") == 0)
                {
                    mt.data = malloc(2000000);
                    assert(mt.data != NULL);
                    memset(mt.data, 'a', 2000000);
                    mt.sz = 2000000;
                }
                else
                {
                    mt.data = strdup(data);
                    mt.sz = json_object_get_string_len(val);
                }

                assert(mt.name != NULL && mt.data != NULL);
                assert(xarray_add_mime_type(&source->mime_types, mt));
            }

            uint32_t serial = wl_display_next_serial(display);

            // Set seat selection
            if (regular)
                wlr_seat_set_selection(
                    seat->seat, (struct wlr_data_source *)source, serial
                );
            else
                wlr_seat_set_primary_selection(
                    seat->seat,
                    (struct wlr_primary_selection_source *)source,
                    serial
                );
            printf("\"OK\"\n");
        }
        // Get contents of mime type for given seat and selection. return JSON
        // null if cleared.
        //
        // Do not call when compositor owns the selection!
        else if (strcmp(cmd, "get") == 0)
        {
            fprintf(stderr, "Getting clipboard\n");

            struct wl_array *mime_types = NULL;

            if (regular)
            {
                if (seat->seat->selection_source != NULL)
                    mime_types = &seat->seat->selection_source->mime_types;
            }
            else
            {
                if (seat->seat->primary_selection_source != NULL)
                    mime_types =
                        &seat->seat->primary_selection_source->mime_types;
            }

            if (mime_types == NULL)
            {
                printf("%s\n", json_object_to_json_string(NULL));
                return true;
            }

            struct json_object *resp = json_object_new_object();

            assert(resp != NULL);

            struct xarray_char arr;
            char             **mime_type = NULL;

            xarray_init_char(&arr);

            wl_array_for_each(mime_type, mime_types)
            {
                int fds[2];

                assert(pipe(fds) != -1);

                if (regular)
                    wlr_data_source_send(
                        seat->seat->selection_source, *mime_type, fds[1]
                    );
                else
                    wlr_primary_selection_source_send(
                        seat->seat->primary_selection_source, *mime_type, fds[1]
                    );
                close(fds[1]);

                wl_display_flush_clients(display);

                // Read until EOF
                static char buf[256];
                while (true)
                {
                    ssize_t r = read(fds[0], buf, sizeof(buf));

                    assert(r != -1 || errno == EINTR);
                    if (r == -1 && errno == EINTR)
                        continue;
                    if (r == 0)
                        break;

                    assert(xarray_concat_char(&arr, buf, r));
                }

                uint32_t sz = xarray_len_char(&arr);
                char    *data = xarray_data_char(&arr);

                struct json_object *j_mime_type =
                    json_object_new_string_len(data, sz);

                json_object_object_add(resp, *mime_type, j_mime_type);

                xarray_clear_char(&arr);
            }
            xarray_uninit_char(&arr);

            printf("%s\n", json_object_to_json_string(resp));
            json_object_put(resp);
        }
        else if (strcmp(cmd, "clear") == 0)
        {
            fprintf(stderr, "Clearing clipboard\n");

            uint32_t serial = wl_display_next_serial(display);

            if (regular)
                wlr_seat_set_selection(seat->seat, NULL, serial);
            else
                wlr_seat_set_primary_selection(seat->seat, NULL, serial);
            printf("\"OK\"\n");
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
        if (execute_command(obj))
            fprintf(stderr, "Success\n");
        else
            wlr_log(WLR_ERROR, "Invalid JSON input");
        json_object_put(obj);
    }
    else
        wlr_log(WLR_ERROR, "JSON parsing error");

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

    assert(display_name != NULL);

    fprintf(stderr, "Starting Wayland display at \"%s\"\n", display_name);

    // Make stdout line buffered
    assert(setvbuf(stdout, NULL, _IOLBF, 0) == 0);

    wlr_log_init(WLR_INFO, NULL);

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

    printf("Ready\n");
    wl_display_run(display);

    for (int i = 0; i < N_ELEMENTS(sources); i++)
        wl_event_source_remove(sources[i]);

    wl_display_destroy_clients(display);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);

    return EXIT_SUCCESS;
}
