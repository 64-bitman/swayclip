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

static void
help(void)
{
    printf("Usage: swctl [OPTIONS] <COMMAND>\n");
    printf("\n");
    printf("Commands:\n");
    printf("  list      List entries in clipboard history\n");
    printf("  get       Get information of entry\n");
    printf("  set       Set current entry\n");
    printf("  delete    Delete entry\n");
    printf("  pin       Pin entry\n");
    printf("  events    Listen for events\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help        Show this help message\n");
    printf("  -v, --version     Show version\n");
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

/*
 * Ownership of "req" is taken. Return true on success and false on fatal error.
 */
static bool
roundtrip(struct json_object *req, struct message *msg)
{
    ipc_ct_write_msg(&ict, IPC_MESSAGE_CALL, req, -1);
    while (ipc_ct_has_pending_writes(&ict))
        if (!ipc_ct_write(&ict))
            return false;

    memset(msg, 0, sizeof(*msg));

    while (msg->obj == NULL)
    {
        struct pollfd pfd = {.fd = ict.fd, .events = POLLIN};

        int ret = poll(&pfd, 1, -1);

        if (ret == -1)
        {
            if (errno == EINTR)
                continue;
            log_errerror("Error reading IPC response");
            return false;
        }

        if (pfd.revents & POLLIN)
            if (!ipc_ct_read(&ict, true, message_callback, msg))
                return false;
    }

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
    // Worst case: every char becomes 4 chars (\xHH), plus null terminator
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
    printf("  -j, --json        Output JSON\n");
    printf("  -s, --start       Position to start at (default=0)\n");
    printf("  -n, --number      Number of entries to list (default=INT64_MAX)\n");
    printf("  -h, --help        Show this help message\n");
    // clang-format on
}

static bool
command_list(int argc, char **argv)
{
    static const struct option options[] = {
        {"json", no_argument, 0, 'j'},
        {"start", required_argument, 0, 's'},
        {"number", required_argument, 0, 'n'},
        {"help", required_argument, 0, 'h'},
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

    struct message resp;

    CHECK(roundtrip(
        build_json_object(
            NULL,
            -1,
            JSON_STR("type", IPC_REQ_GET_ENTRIES),
            JSON_INT("start", start),
            JSON_INT("n", number),
            NULL
        ),
        &resp
    ));

    CHECK(json_object_is_type(resp.obj, json_type_array));

    if (json)
    {
        printf("%s\n", json_object_to_json_string(resp.obj));
        goto exit;
    }

    for (size_t i = 0; i < json_object_array_length(resp.obj); i++)
    {
        struct json_object *entry = json_object_array_get_idx(resp.obj, i);

        CHECK(json_object_is_type(entry, json_type_object));

        int64_t     id;
        const char *content_type;
        const char *mime_type;

        CHECK(extract_json_object(
            entry,
            JSON_INT("id", &id),
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

        if (strcmp(content_type, "binary") == 0)
        {
            printf(
                "%" PRId64 "\t[[ binary data %s %s ]]\n",
                id,
                mime_type,
                human_readable_size(data_resp.aux_data_len)
            );
        }
        else if (strcmp(content_type, "text") == 0)
        {
            // Set cap at 100 chars/bytes
            char *str = text_escape(
                (char *)data_resp.aux_data,
                MIN((size_t)100, data_resp.aux_data_len)
            );

            printf("%" PRId64 "\t%s\n", id, str);
            free(str);
        }
        else if (strcmp(content_type, "image") == 0)
        {
            printf(
                "%" PRId64 "\t[[ image data %s %s ]]\n",
                id,
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
    printf("  -h, --help        Show this help message\n");
    // clang-format on
}

static bool
command_len(int argc, char **argv)
{
    static const struct option options[] = {
        {"help", required_argument, 0, 'h'}, {NULL, 0, 0, 0}
    };

    int c;
    int idx;

    while ((c = getopt_long(argc, argv, "s:n:h", options, &idx)) != -1)
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

    log_init(NULL);

    if (optind >= argc)
    {
        log_error("No subcommand given");
        return EXIT_FAILURE;
    }

    bool ret = true, unknown = false;

    const char *cmd = argv[optind];

    int    sub_argc = argc - optind;
    char **sub_argv = argv + optind;
    optind = 1;

    if (strcmp(cmd, "list") == 0)
        ret = command_list(sub_argc, sub_argv);
    else if (strcmp(cmd, "len") == 0)
        ret = command_len(sub_argc, sub_argv);
    else
    {
        log_error("Unknown subcommand \"%s\"", cmd);
        ret = false;
        unknown = true;
    }

    if (!unknown)
        ipc_ct_uninit(&ict);

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}
