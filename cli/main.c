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

#include "common/io.h"
#include "common/ipc_ct.h"
#include "common/json_util.h"
#include "common/log.h"
#include "common/util.h"
#include "version.h"
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Used like assert() but doesn't abort program
#define CHECK(expr)                                                            \
    do                                                                         \
    {                                                                          \
        if (!(expr))                                                           \
        {                                                                      \
            log_error("Expression \"" STRINGIFY(expr) "\" failed");            \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (false)

static struct ipc_ct ict;
static bool          ict_init = false;

xarray_create(char, input, uint32_t, 256, 2.0);

static void
help(void)
{
    printf("Usage: swctl [OPTIONS] <COMMAND>\n");
    printf("\n");
    printf("Commands:\n");
    printf("  list      List entries in clipboard history.\n");
    printf("  len       Get number of entries in history.\n");
    printf("  set       Set current entry.\n");
    printf("  get       Get contents of entry.\n");
    printf("  delete    Delete entry.\n");
    printf("  pin       Pin an entry.\n");
    printf("  current   Get of ID of entry clipboard is set to.\n");
    printf("  events    Listen for events.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message.\n");
    printf("  -v, --version     Show version.\n");
}

static bool
init_ipc(struct ipc_ct *ict)
{
    char *path = get_ipc_path();

    if (path == NULL)
        return false;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd == -1)
    {
        log_errerror("Error creating socket descriptor");
        free(path);
        return false;
    }

    struct sockaddr_un addr;

    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        log_errerror("Error connecting to socket \"%s\"", path);
        free(path);
        close(fd);
        return false;
    }
    free(path);

    if (!ipc_ct_init(ict, fd))
    {
        close(fd);
        return false;
    }

    if (!set_fd_nonblocking(fd))
    {
        close(fd);
        return false;
    }

    ict_init = true;
    return true;
}

struct message
{
    struct json_object *obj;

    uint8_t *aux_data;
    size_t   aux_data_len;
};

static void
message_clear(struct message *msg)
{
    json_object_put(msg->obj);
    free(msg->aux_data);
}

static void
message_callback(struct ipc_message *imsg, void *udata)
{
    struct message *msg = udata;

    // TODO: maybe don't copy the auxillary data every time?
    msg->obj = json_object_get(imsg->payload);
    msg->aux_data = malloc(imsg->aux_data_len);
    if (msg->aux_data != NULL)
    {
        msg->aux_data_len = imsg->aux_data_len;
        memcpy(msg->aux_data, imsg->aux_data, imsg->aux_data_len);
    }
    else
        log_warn("Error allocating auxillary data");
}

static bool
read_msgs(ipc_msg_callback callback, void *udata)
{
    struct pollfd pfd = {.fd = ict.fd, .events = POLLIN};

    int ret = poll(&pfd, 1, -1);

    if (ret == -1)
    {
        if (errno == EINTR)
            return true;
        log_errerror("Error reading IPC response");
        return false;
    }

    if (pfd.revents & POLLIN)
        if (!ipc_ct_read(&ict, IPC_CT_WANT_SCM_FD, callback, udata))
            return false;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
    {
        log_error("IPC connection closed");
        return false;
    }
    return true;
}

/*
 * Ownership of "req" is taken. Return true on success and false on fatal error.
 */
static bool
roundtrip(struct json_object *req, struct message *msg)
{
    ipc_ct_write_msg(&ict, IPC_MESSAGE_CALL, req, -1);
    while (ipc_ct_has_pending_writes(&ict))
    {
        bool need_poll = false;
        if (!ipc_ct_write(&ict, &need_poll))
        {
            if (need_poll)
            {
                struct pollfd pfd = {.fd = ict.fd, .events = POLLOUT};

                while (true)
                {
                    int ret = poll(&pfd, 1, -1);

                    if (ret == -1)
                    {
                        if (errno == EINTR)
                            continue;
                        log_errerror("Error polling IPC connection");
                        return false;
                    }
                    if (pfd.revents & POLLOUT)
                        break;
                }
                continue;
            }
            return false;
        }
    }

    memset(msg, 0, sizeof(*msg));

    while (msg->obj == NULL)
        if (!read_msgs(message_callback, msg))
            return false;

    return true;
}

static const char *
human_readable_size(size_t bytes)
{
    static char buf[256];

    const char *units[] = {"B", "KB", "MB", "GB"};
    int         unit_index = 0;
    double      size = (double)bytes;

    while (size >= 1024.0 && unit_index < 3)
    {
        size /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0)
        // Bytes: no decimal point
        snprintf(buf, sizeof(buf), "%zu %s", bytes, units[unit_index]);
    else
        snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit_index]);

    return buf;
}

/*
 * Converts control characters to visible text representation. Returns allocated
 * string.
 */
char *
text_escape(const char *str, size_t len)
{
    // Worst case: every char becomes 4 chars (\xHH), plus NUL terminator
    char *out = malloc(len * 4 + 1);

    assert(out != NULL);

    size_t j = 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)str[i];

        switch (c)
        {
        case '\a':
            out[j++] = '\\';
            out[j++] = 'a';
            break;
        case '\b':
            out[j++] = '\\';
            out[j++] = 'b';
            break;
        case '\f':
            out[j++] = '\\';
            out[j++] = 'f';
            break;
        case '\n':
            out[j++] = '\\';
            out[j++] = 'n';
            break;
        case '\r':
            out[j++] = '\\';
            out[j++] = 'r';
            break;
        case '\t':
            out[j++] = '\\';
            out[j++] = 't';
            break;
        case '\v':
            out[j++] = '\\';
            out[j++] = 'v';
            break;
        case '\\':
            out[j++] = '\\';
            out[j++] = '\\';
            break;
        default:
            if (c < 0x20 || c == 0x7F)
            {
                // Other control chars: use \xHH notation
                j += sprintf(&out[j], "\\x%02X", c);
            }
            else
            {
                out[j++] = (char)c;
            }
        }
    }
    out[j] = '\0';
    return out;
}

/*
 * Return true is JSON response is success
 */
bool
is_success(struct json_object *resp)
{
    struct json_object *j_success;

    json_object_object_get_ex(resp, "type", &j_success);
    CHECK(json_object_is_type(j_success, json_type_string));

    if (strcmp(json_object_get_string(j_success), "error") == 0)
    {
        const char *desc;

        CHECK(extract_json_object(resp, JSON_STR("desc", &desc), NULL));
        log_error("IPC error: %s", desc);

        return false;
    }
    return strcmp(json_object_get_string(j_success), "success") == 0;
}

/*
 * Read stdin until EOF
 */
struct xarray_input
read_stdin(void)
{
    static char buf[256];

    struct xarray_input arr;

    xarray_init_input(&arr);

    while (true)
    {
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));

        CHECK(r != -1 && errno != EINTR);
        if (r == -1 && errno == EINTR)
            continue;
        if (r == 0)
            break;
        CHECK(xarray_concat_input(&arr, buf, r));
    }
    return arr;
}

static void
help_list(void)
{
    // clang-format off
    printf("Usage: swctl list [OPTIONS]\n");
    printf("\n");
    printf("List up to \"--number\" entries starting at \"--start\",\n"
            "in format for use by pickers, or as a JSON array.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -j, --json        Output JSON.\n");
    printf("  -s, --start       Position to start at (default=0).\n");
    printf("  -n, --number      Number of entries to list (default=INT64_MAX).\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
print_entry(int64_t start, int64_t n, bool json, bool print_empty)
{
    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_GET_HISTORY),
            JSON_INT("start", start),
            JSON_INT("n", n),
            NULL
        ),
        &resp
    ));

    CHECK(json_object_is_type(resp.obj, json_type_array));

    if (json_object_array_length(resp.obj) == 0)
    {
        if (print_empty && json)
            printf("[]\n");
        return true;
    }

    bool last = json_object_array_length(resp.obj) != (size_t)n;

    if (json)
    {
        printf(
            "%s\n",
            json_object_to_json_string_ext(resp.obj, JSON_C_TO_STRING_PLAIN)
        );
        goto exit;
    }

    for (size_t i = 0; i < json_object_array_length(resp.obj); i++)
    {
        struct json_object *entry = json_object_array_get_idx(resp.obj, i);

        CHECK(json_object_is_type(entry, json_type_object));

        int64_t     id;
        bool        pinned;
        bool        current;
        const char *content_type;
        const char *mime_type;

        CHECK(extract_json_object(
            entry,
            JSON_INT("id", &id),
            JSON_BOOL("pinned", &pinned),
            JSON_BOOL("current", &current),
            JSON_STR("content_type", &content_type),
            JSON_STR("content_mime_type", &mime_type),
            NULL
        ));

        struct message data_resp;

        CHECK(roundtrip(
            build_json_object(
                NULL,
                -1,
                JSON_STR("type", IPC_REQ_GET_DATA),
                JSON_INT("id", id),
                JSON_STR("mime_type", mime_type),
                NULL
            ),
            &data_resp
        ));

        static char aux[128];

        snprintf(
            aux,
            sizeof(aux),
            "%s%s",
            current ? "[CURRENT] " : "",
            pinned ? "[PINNED] " : ""
        );

        if (strcmp(content_type, "binary") == 0)
        {
            printf(
                "%" PRId64 "\t%s[[ binary data %s %s ]]\n",
                id,
                aux,
                mime_type,
                human_readable_size(data_resp.aux_data_len)
            );
        }
        else if (strcmp(content_type, "text") == 0)
        {
            // Set cap at 100 chars/bytes
            char *stuff = (char *)data_resp.aux_data;

            if (stuff == NULL)
            {
                printf(
                    "%" PRId64 "\t[[ error %s %s ]]\n",
                    id,
                    mime_type,
                    human_readable_size(data_resp.aux_data_len)
                );
                break;
            }

            char *str =
                text_escape(stuff, MIN((size_t)100, data_resp.aux_data_len));

            printf("%" PRId64 "\t%s%s\n", id, aux, str);
            free(str);
        }
        else if (strcmp(content_type, "image") == 0)
        {
            printf(
                "%" PRId64 "\t%s[[ image data %s %s ]]\n",
                id,
                aux,
                mime_type,
                human_readable_size(data_resp.aux_data_len)
            );
        }
        else
        {
            printf(
                "%" PRId64 "\t[[ unknown data %s ]]\n",
                id,
                human_readable_size(data_resp.aux_data_len)
            );
        }

        message_clear(&data_resp);
    }

exit:
    message_clear(&resp);

    return last;
}

static bool
command_list(int argc, char **argv)
{
    static const struct option options[] = {
        {"json", no_argument, 0, 'j'},
        {"start", required_argument, 0, 's'},
        {"number", required_argument, 0, 'n'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    bool    json = false;
    int64_t start = 0;
    int64_t number = INT64_MAX;

    while ((c = getopt_long(argc, argv, "js:n:h", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'j':
            json = true;
            break;
        case 's':
            start = strtoll(optarg, NULL, 10);
            break;
        case 'n':
            number = strtoll(optarg, NULL, 10);
            break;
        case 'h':
            help_list();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    bool    first = false;
    int64_t off = start;
    int64_t remain = number;

    // Incrementally receive entries 100 at a time
    while (remain > 0 && !print_entry(off, MIN(100, remain), json, !first))
    {
        first = true;
        off += 100;
        remain -= 100;
    }

    return true;
}

static void
help_len(void)
{
    // clang-format off
    printf("Usage: swctl len [OPTIONS]\n");
    printf("\n");
    printf("Output number of entries in clipboard history.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_len(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, 0, 'h'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "h", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'h':
            help_len();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL, -1, JSON_STR("type", IPC_REQ_GET_HISTORY_LENGTH), NULL
        ),
        &resp
    ));

    CHECK(json_object_is_type(resp.obj, json_type_object));

    int64_t size;

    CHECK(extract_json_object(resp.obj, JSON_INT("size", &size), NULL));

    printf("%" PRId64 "\n", size);

    message_clear(&resp);

    return true;
}

static void
help_set(void)
{
    // clang-format off
    printf("Usage: swctl set [OPTIONS] [ID]\n");
    printf("\n");
    printf("Set clipboard to entry\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --clear       Clear clipboard.\n");
    printf("  -d, --decode      Decode format used by \"swctl list\" from stdin.\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_set(int argc, char **argv)
{
    static const struct option options[] = {
        {"clear", no_argument, 0, 'c'},
        {"decode", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    bool    clear = false;
    bool    decode = false;
    int64_t id = -1;

    while ((c = getopt_long(argc, argv, "+cdh", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'c':
            clear = true;
            break;
        case 'd':
            decode = true;
            break;
        case 'h':
            help_set();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    if (decode)
    {
        struct xarray_input in = read_stdin();

        if (xarray_len_input(&in) == 0)
            // Ignore empty input
            return false;

        id = strtol(xarray_data_input(&in), NULL, 10);
        xarray_uninit_input(&in);
    }
    else if (!clear)
    {
        if (argv[optind] == NULL)
            return false;
        id = strtol(argv[optind], NULL, 10);
    }

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_SET_CLIPBOARD),
            JSON_INT("id", id),
            NULL
        ),
        &resp
    ));

    bool success = is_success(resp.obj);

    message_clear(&resp);

    return success;
}

static void
help_get(void)
{
    // clang-format off
    printf("Usage: swctl get [OPTIONS] <ID> <MIME TYPE>\n");
    printf("\n");
    printf("Get contents of mime type for entry\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_get(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, 0, 'h'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "+dh", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'h':
            help_get();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    if (argv[optind] == NULL || argv[optind + 1] == NULL)
        return false;

    int64_t id = strtol(argv[optind], NULL, 10);

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_GET_DATA),
            JSON_INT("id", id),
            JSON_STR("mime_type", argv[optind + 1]),
            NULL
        ),
        &resp
    ));

    bool success = is_success(resp.obj);

    // Not sure if aux_data can be NULL...
    if (resp.aux_data != NULL)
        fwrite(resp.aux_data, 1, resp.aux_data_len, stdout);
    else
        success = false;

    message_clear(&resp);

    return success;
}

static void
help_delete(void)
{
    // clang-format off
    printf("Usage: swctl delete [OPTIONS] [ID]\n");
    printf("\n");
    printf("Delete entry with ID from clipboard history.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --clear       Clear clipboard history.\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_delete(int argc, char **argv)
{
    static const struct option options[] = {
        {"clear", no_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    bool clear = false;

    while ((c = getopt_long(argc, argv, "+ch", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'c':
            clear = true;
            break;
        case 'h':
            help_delete();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    int64_t id = -1;

    if (!clear)
    {
        if (argv[optind] == NULL)
            return false;
        id = strtol(argv[optind], NULL, 10);
    }

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_DELETE_ENTRY),
            JSON_INT("id", id),
            NULL
        ),
        &resp
    ));

    bool success = is_success(resp.obj);

    message_clear(&resp);

    return success;
}

static void
help_pin(void)
{
    // clang-format off
    printf("Usage: swctl pin [OPTIONS] <ID>\n");
    printf("\n");
    printf("Pin entry to prevent it from being automatically removed.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -u, --unpin       Unpin entry.\n");
    printf("  -t, --toggle      Toggle pinned status of entry.\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_pin(int argc, char **argv)
{
    static const struct option options[] = {
        {"unpin", no_argument, 0, 'u'},
        {"toggle", no_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    bool pin = true;
    bool toggle = false;

    while ((c = getopt_long(argc, argv, "+uth", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'u':
            pin = false;
            break;
        case 't':
            toggle = true;
            break;
        case 'h':
            help_pin();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    if (argv[optind] == NULL)
        return false;

    int64_t id = strtol(argv[optind], NULL, 10);

    struct message resp;

    const char *pinstr;

    if (toggle)
        pinstr = "toggle";
    else if (pin)
        pinstr = "yes";
    else
        pinstr = "no";

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_PIN_ENTRY),
            JSON_INT("id", id),
            JSON_STR("pin", pinstr),
            NULL
        ),
        &resp
    ));

    bool success = is_success(resp.obj);

    message_clear(&resp);

    return success;
}

static void
help_current(void)
{
    // clang-format off
    printf("Usage: swctl current [OPTIONS]\n");
    printf("\n");
    printf("Output of ID of entry clipboard is set to, or -1 if clipboard is cleared.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static bool
command_current(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, 0, 'h'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "h", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'h':
            help_current();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL, -1, JSON_STR("type", IPC_REQ_GET_CURRENT), NULL
        ),
        &resp
    ));

    CHECK(json_object_is_type(resp.obj, json_type_int));

    printf("%" PRId64 "\n", json_object_get_int64(resp.obj));

    message_clear(&resp);

    return true;
}

static void
help_events(void)
{
    // clang-format off
    printf("Usage: swctl events [OPTIONS] <events>\n");
    printf("\n");
    printf("Output JSON stream of events.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message.\n");
    // clang-format on
}

static void
event_callback(struct ipc_message *imsg, void *udata UNUSED)
{
    printf(
        "%s\n",
        json_object_to_json_string_ext(imsg->payload, JSON_C_TO_STRING_PLAIN)
    );
}

static bool
command_events(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, 0, 'h'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "h", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'h':
            help_events();
            return true;
        default:
            return false;
        }
    }

    if (!init_ipc(&ict))
        return false;

    struct json_object *arr = json_object_new_array();

    CHECK(arr != NULL);

    for (char **p = argv + optind; *p != NULL; p++)
        json_object_array_add(arr, json_object_new_string(*p));

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_SUBSCRIBE),
            JSON_OBJ("events", arr),
            NULL
        ),
        &resp
    ));

    bool success = is_success(resp.obj);

    message_clear(&resp);

    if (!success)
        return false;

    while (true)
        if (!read_msgs(event_callback, NULL))
            return false;

    return true;
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "+hv", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'h':
            help();
            return EXIT_SUCCESS;
        case 'v':
            printf("%s\n", PROJECT_VERSION);
            return EXIT_SUCCESS;
        default:
            return EXIT_FAILURE;
        }
    }

    log_init(NULL, false);

    if (optind >= argc)
    {
        log_error("No subcommand given");
        return EXIT_FAILURE;
    }

    bool ret = true;

    const char *cmd = argv[optind];

    int    sub_argc = argc - optind;
    char **sub_argv = argv + optind;
    optind = 1;

    if (strcmp(cmd, "list") == 0)
        ret = command_list(sub_argc, sub_argv);
    else if (strcmp(cmd, "len") == 0)
        ret = command_len(sub_argc, sub_argv);
    else if (strcmp(cmd, "set") == 0)
        ret = command_set(sub_argc, sub_argv);
    else if (strcmp(cmd, "get") == 0)
        ret = command_get(sub_argc, sub_argv);
    else if (strcmp(cmd, "delete") == 0)
        ret = command_delete(sub_argc, sub_argv);
    else if (strcmp(cmd, "pin") == 0)
        ret = command_pin(sub_argc, sub_argv);
    else if (strcmp(cmd, "current") == 0)
        ret = command_current(sub_argc, sub_argv);
    else if (strcmp(cmd, "events") == 0)
        ret = command_events(sub_argc, sub_argv);
    else
    {
        log_error("Unknown subcommand \"%s\"", cmd);
        ret = false;
    }

    if (ict_init)
        ipc_ct_uninit(&ict);

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}
