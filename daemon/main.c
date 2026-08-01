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

#include "common/event.h"
#include "common/io.h"
#include "common/json_util.h"
#include "common/log.h"
#include "common/sha256/sha256.h"
#include "common/version.h"
#include "config.h"
#include "database.h"
#include "ipc.h"
#include "wayland.h"
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/signalfd.h>

#define IPC_INVALID_ARGS "Invalid arguments"
#define IPC_DB_ERROR "Database error"
#define IPC_MEM_ERROR "Memory error"

// Content types with a larger value are prioritized over others (except
// CONTENT_UNKNOWN).
enum content_type
{
    CONTENT_UNKNOWN,
    CONTENT_BINARY,
    CONTENT_TEXT,
    CONTENT_IMAGE
};

static const char *content_names[] = {
    [CONTENT_UNKNOWN] = "unknown",
    [CONTENT_BINARY] = "binary",
    [CONTENT_TEXT] = "text",
    [CONTENT_IMAGE] = "image"
};

struct state
{
    struct eventloop loop;

    struct config config;

    struct wayland  wayland;
    struct database db;
    struct ipc      ipc;

    uint8_t buf[4096]; // Used for I/O operations

    // ID of current entry that all enabled selections are synced to. -1 if no
    // entry set, or if "cleared" is true, then clipboard is cleared.
    int64_t id;
    bool    cleared;
};

static bool
signal_callback(int fd, int events UNUSED, void *udata)
{
    struct signalfd_siginfo sfd_info;

    ssize_t r = read(fd, &sfd_info, sizeof(sfd_info));

    if (r == -1)
    {
        log_errwarn("Error reading signal fd");
        return false;
    }

    if (sfd_info.ssi_signo == SIGINT || sfd_info.ssi_signo == SIGTERM)
    {
        struct eventloop *loop = udata;

        eventloop_stop(loop);
        return true;
    }
    return false;
}

static bool
update(struct state *state, int64_t id, const bool *pinned)
{
    int64_t t = database_update_entry(&state->db, id, pinned);

    if (t == -1)
        return false;

    ipc_event_entry_update(&state->ipc, id, &t, pinned);
    return true;
}

static void
set(struct state *state, struct selection *ignore)
{
    if (state->id != -1)
        ipc_event_clipboard_state(&state->ipc, state->id, false);

    if (database_save_setting(
            &state->db, DB_SETTING_LAST_ENTRY, 'i', state->id
        ))
        wayland_set(&state->wayland, ignore);
    else
        state->id = -1;

    if (state->id != -1)
    {
        ipc_event_clipboard_state(&state->ipc, state->id, true);
        update(state, state->id, NULL);
    }
}

static void
read_callback(uint8_t *buf, ssize_t r, void *udata)
{
    SHA256_CTX *sha_ctx = udata;

    sha256_update(sha_ctx, (BYTE *)buf, r);
}

/*
 * Get the content type from the mime type.
 */
static enum content_type
get_content_type(const char *mime_type)
{
    if (strncmp(mime_type, "image/", 6) == 0)
        return CONTENT_IMAGE;
    if (strncmp(mime_type, "text/", 5) == 0)
        return CONTENT_TEXT;
    return CONTENT_BINARY;
}

static void
wsignal_selection(
    struct selection                 *sel,
    struct ext_data_control_offer_v1 *offer,
    struct xarray_mime_type          *mime_types,
    void                             *udata
)
{
    struct state *state = udata;

    if (xarray_len_mime_type(mime_types) == 0)
    {
        log_debug("Selection event has no allowed mime types, ignoring");
        return;
    }

    if (!database_do_transaction(&state->db, DB_TRANSACTION_IMMEDIATE))
        return;

    // SHA256 hash used to represent this selection event.
    SHA256_CTX sha_ctx;
    int64_t    id = database_new_entry(&state->db);
    bool       ret = false;
    bool       ignore = false;
    bool       moved = false;
    int64_t    old_pos; // Only set if "mvoed" is true

    if (id == -1)
        goto exit;

    sha256_init(&sha_ctx);

    char   *mime_type;
    uint8_t data_id[SHA256_BLOCK_SIZE];

    enum content_type ctype = CONTENT_UNKNOWN;
    const char       *content_mime = NULL;

    // Receive the contents of every mime type and add them to the database. The
    // filtering of mime types is done in wayland.c
    xarray_foreach_val(mime_type, mime_types, mime_type)
    {
        int fd = wayland_get_offer_fd(&state->wayland, offer, mime_type);

        if (fd == -1)
            goto exit;

        if (!set_fd_nonblocking(fd))
            goto exit;

        SHA256_CTX data_sha_ctx;

        sha256_init(&data_sha_ctx);

        struct io_read ctx = {
            .fd = fd,
            .buf = state->buf,
            .bufsize = sizeof(state->buf),
            .data_callback = read_callback,
            .callback_udata = &data_sha_ctx,
        };

        if (!io_read(&ctx, 3000, state->config.max_size))
        {
            close(fd);
            goto exit;
        }

        close(fd);

        struct xarray_io *data = &ctx.data;

        sha256_final(&data_sha_ctx, data_id);

        sha256_update(&sha_ctx, (BYTE *)mime_type, strlen(mime_type));
        sha256_update(&sha_ctx, data_id, SHA256_BLOCK_SIZE);

        bool ret = database_new_mime_type(
            &state->db,
            id,
            mime_type,
            data_id,
            xarray_data_io(data),
            xarray_len_io(data)
        );

        xarray_uninit_io(data);
        if (!ret)
            goto exit;

        enum content_type m_ctype = get_content_type(mime_type);

        if (m_ctype > ctype)
        {
            ctype = m_ctype;
            content_mime = mime_type;
        }
    }

    uint8_t entry_hash[SHA256_BLOCK_SIZE];

    sha256_final(&sha_ctx, entry_hash);

    switch (state->config.dedup)
    {
    case DEDUP_NONE:
        // Add entry normally
        ret = database_set_entry_hash(&state->db, -1, entry_hash);
        break;
    case DEDUP_PREV:
    {
        int64_t pos = database_entry_hash_pos(&state->db, entry_hash, NULL);

        if (pos == -1)
        {
            // Nothing to deduplicate
            ret = true;
            break;
        }

        pos--;
        if (pos == 0)
        {
            // Previous entry has same entry hash, ignore
            ret = false;
            ignore = true;
        }
        else if (pos > 0)
            // Entry with the same hash exists but it is not the previous one,
            // set its hash to NULL so that UNIQUE constraint is ok.
            ret = database_set_entry_hash(&state->db, -1, entry_hash);
        break;
    }
    case DEDUP_ALL:
    {
        int64_t dup_id;
        old_pos = database_entry_hash_pos(&state->db, entry_hash, &dup_id);

        if (old_pos == -1)
        {
            // Nothing to deduplicate
            ret = true;
            break;
        }

        // Must exclude the currently in progress entry, since that will offset
        // the position by 1, so subtract 1.
        old_pos--;

        if (old_pos == 0)
        {
            // Previous entry has same hash, just ignore, no need to move
            ret = false;
            ignore = true;
            break;
        }

        // Move entry to front of history. Must do this later, so we can
        // rollback the current transaction.
        moved = true;
        ret = false;
        id = dup_id;
        break;
    }
    default:
        log_abort("Unknown dedup value %d", state->config.dedup);
    }

    if (ret && !database_set_entry_hash(&state->db, id, entry_hash))
        ret = false;

exit:
    database_do_transaction(
        &state->db, ret ? DB_TRANSACTION_COMMIT : DB_TRANSACTION_ROLLBACK
    );

    if (moved)
    {
        ret = database_update_sort_index(&state->db, id);
        ignore = true;
    }

    if (ret)
    {
        if (moved)
        {
            ipc_event_entry_move(&state->ipc, id, old_pos);
        }
        else
        {
            // Save content type and mime type for that content type
            assert(content_mime != NULL);
            (void)database_save_attribute(
                &state->db,
                id,
                DB_ATTRIBUTE_CONTENT_TYPE,
                's',
                content_names[ctype]
            );
            (void)database_save_attribute(
                &state->db, id, DB_ATTRIBUTE_CONTENT_MIME, 's', content_mime
            );

            ipc_event_entry_add(&state->ipc, id);
        }
        state->id = id;

        // Do not become the source client for the selection that the event came
        // from.
        set(state, sel);
    }
    else if (!ignore)
        state->id = -1;
}

static bool
write_callback(uint8_t *buf, size_t sz, size_t *len, void *udata)
{
    struct state *state = ((void **)udata)[0];
    sqlite3_blob *blob = ((void **)udata)[1];
    int           blob_sz = *(int *)((void **)udata)[2];
    int          *off = ((void **)udata)[3];

    *len = MIN((size_t)blob_sz - *off, sz);
    if (sqlite3_blob_read(blob, buf, *len, *off) != SQLITE_OK)
    {
        log_error(
            "Error reading from blob: %s", sqlite3_errmsg(state->db.handle)
        );
        return false;
    }
    *off += *len;

    return true;
}

static void
wsignal_send(const char *mime_type, int fd, void *udata)
{
    struct state *state = udata;

    if (state->id == -1)
        return;
    if (!set_fd_nonblocking(fd))
        return;

    sqlite3_blob *blob = database_get_data(&state->db, state->id, mime_type);

    if (blob == NULL)
        return;

    int sz = sqlite3_blob_bytes(blob);
    int off = 0;

    void           *callback_udata[] = {state, blob, &sz, &off};
    struct io_write ctx = {
        .fd = fd,
        .buf = state->buf,
        .bufsize = sizeof(state->buf),
        .data_callback = write_callback,
        .callback_udata = callback_udata
    };

    // TODO: Maybe run this in the event loop (non-blocking)?
    (void)io_write(&ctx, 3000);

    sqlite3_blob_close(blob);
}

static void
send_mime_type_callback(const char *mime_type, void *udata)
{
    struct ext_data_control_source_v1 *source = udata;

    ext_data_control_source_v1_offer(source, mime_type);
}

static struct ext_data_control_source_v1 *
wsignal_set(struct ext_data_control_manager_v1 *mgr, bool *clear, void *udata)
{
    struct state *state = udata;

    struct ext_data_control_source_v1 *source = NULL;

    if (state->id != -1)
    {
        source = ext_data_control_manager_v1_create_data_source(mgr);
        (void)database_get_mime_types(
            &state->db, state->id, send_mime_type_callback, source
        );
    }
    else if (state->cleared)
        *clear = true;

    return source;
}

static bool
wsignal_can_set(void *udata)
{
    struct state *state = udata;

    return state->id != -1;
}

static void
mime_type_callback(const char *mime_type, void *udata)
{
    struct json_object *arr = udata;
    struct json_object *j = json_object_new_string(mime_type);

    if (j == NULL)
        return;
    if (json_object_array_add(arr, j) == -1)
        json_object_put(j);
}

static void
get_entry_callback(
    int64_t id,
    int64_t creation_time,
    int64_t update_time,
    bool    pinned,
    void   *udata
)
{
    struct json_object *arr = ((void **)udata)[0];
    struct database    *db = ((void **)udata)[1];
    struct json_object *mime_types = json_object_new_array();
    struct json_object *obj = json_object_new_object();

    if (mime_types == NULL)
        return;
    if (obj == NULL)
    {
        json_object_put(mime_types);
        return;
    }

    if (!database_get_mime_types(db, id, mime_type_callback, mime_types))
    {
        json_object_put(mime_types);
        json_object_put(obj);
        return;
    }

    (void)build_json_object(
        obj,
        -1,
        JSON_INT("id", id),
        JSON_INT("creation_time", creation_time),
        JSON_INT("update_time", update_time),
        JSON_BOOL("pinned", pinned),
        JSON_OBJ("mime_types", mime_types),
        NULL
    );

    // Add content type and content mime type
    static char buf[256];
    if (!database_get_attribute(
            db, id, DB_ATTRIBUTE_CONTENT_TYPE, 's', buf, sizeof(buf)
        ))
    {
        json_object_put(obj);
        return;
    }
    (void)build_json_object(obj, -1, JSON_STR("content_type", buf), NULL);

    if (!database_get_attribute(
            db, id, DB_ATTRIBUTE_CONTENT_MIME, 's', buf, sizeof(buf)
        ))
    {
        json_object_put(obj);
        return;
    }
    (void)build_json_object(obj, -1, JSON_STR("content_mime_type", buf), NULL);

    if (json_object_array_add(arr, obj) == -1)
        json_object_put(obj);
}

static void
request_callback(
    struct ipc_client *client, struct ipc_message *req, void *udata
)
{
    struct state *state = udata;
    const char   *type;

    if (!extract_json_object(req->payload, JSON_STR("type", &type), NULL))
    {
        ipc_client_send_error(client, IPC_INVALID_ARGS);
        return;
    }

    log_debug("Serving IPC request \"%s\"", type);

    if (strcmp(type, IPC_REQ_SUBSCRIBE) == 0)
    {
        struct json_object *arr;

        if (!extract_json_object(
                req->payload, JSON_ARRAY("events", &arr), NULL
            ))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        uint events = 0;

        for (size_t i = 0; i < json_object_array_length(arr); i++)
        {
            struct json_object *j_event = json_object_array_get_idx(arr, i);
            const char         *event = json_object_get_string(j_event);

            if (event == NULL)
            {
                ipc_client_send_error(client, IPC_INVALID_ARGS, event);
                return;
            }

            if (strcmp(event, IPC_EVENT_ENTRY_ADD) == 0)
                events |= IPC_EVENT_FLAG_ENTRY_ADD;
            else if (strcmp(event, IPC_EVENT_ENTRY_DELETE) == 0)
                events |= IPC_EVENT_FLAG_ENTRY_DELETE;
            else if (strcmp(event, IPC_EVENT_ENTRY_UPDATE) == 0)
                events |= IPC_EVENT_FLAG_ENTRY_UPDATE;
            else if (strcmp(event, IPC_EVENT_ENTRY_MOVE) == 0)
                events |= IPC_EVENT_FLAG_ENTRY_MOVE;
            else if (strcmp(event, IPC_EVENT_CLIPBOARD_STATE) == 0)
                events |= IPC_EVENT_FLAG_CLIPBOARD_STATE;
            else
            {
                ipc_client_send_error(client, "Unknown event \"%s\"", event);
                return;
            }
            log_debug("Subscribing to event \"%s\"", event);
        }

        ipc_client_set_events(client, events);
        ipc_client_send_success(client);
    }
    else if (strcmp(type, IPC_REQ_GET_HISTORY_LENGTH) == 0)
    {
        int64_t size = database_get_history_size(&state->db);

        if (size == -1)
            ipc_client_send_error(client, IPC_DB_ERROR);
        else
            ipc_client_send(
                client,
                build_json_object(NULL, -1, JSON_INT("size", size), NULL),
                -1
            );
    }
    else if (strcmp(type, IPC_REQ_GET_ENTRIES) == 0)
    {
        int64_t start;
        int64_t n;

        if (!extract_json_object(
                req->payload, JSON_INT("start", &start), JSON_INT("n", &n), NULL
            ))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        struct json_object *arr = json_object_new_array();

        if (arr == NULL)
        {
            ipc_client_send_error(client, IPC_MEM_ERROR);
            return;
        }

        const void *udata[] = {arr, &state->db};

        if (!database_get_entries(
                &state->db, start, n, get_entry_callback, udata
            ))
        {
            ipc_client_send_error(client, IPC_DB_ERROR);
            return;
        }

        ipc_client_send(client, arr, -1);
    }
    else if (strcmp(type, IPC_REQ_GET_DATA) == 0)
    {
        int64_t     id;
        const char *mime_type;

        if (!extract_json_object(
                req->payload,
                JSON_INT("id", &id),
                JSON_STR("mime_type", &mime_type),
                NULL
            ))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        sqlite3_blob *blob = database_get_data(&state->db, id, mime_type);

        if (blob == NULL)
        {
            ipc_client_send_error(client, IPC_DB_ERROR);
            return;
        }

        int sz = sqlite3_blob_bytes(blob);
        int fd = memfd_create(mime_type, MFD_CLOEXEC | MFD_ALLOW_SEALING);

        if (fd == -1 || ftruncate(fd, sz) == -1)
        {
            ipc_client_send_error(
                client, "Error creating file descriptor: %s", strerror(errno)
            );
            sqlite3_blob_close(blob);
            if (fd != -1)
                close(fd);
            return;
        }

        void *map = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (map == NULL || sqlite3_blob_read(blob, map, sz, 0) != SQLITE_OK)
        {
            ipc_client_send_error(client, "Error mapping file descriptor");
            sqlite3_blob_close(blob);
            if (map != NULL)
                munmap(map, sz);
            close(fd);
            return;
        }

        munmap(map, sz);
        sqlite3_blob_close(blob);

        if (fcntl(
                fd,
                F_ADD_SEALS,
                F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL
            ) == -1)
        {
            ipc_client_send_error(client, "Error sealing file descriptor");
            close(fd);
            return;
        }

        ipc_client_send_success_fd(client, fd);
    }
    else if (strcmp(type, IPC_REQ_SET_CLIPBOARD) == 0)
    {
        int64_t id;

        if (!extract_json_object(req->payload, JSON_INT("id", &id), NULL))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        // Check if ID exists first (unless -1, then clear clipboard)
        if (id != -1 && !database_id_exists(&state->db, id))
        {
            ipc_client_send_error(
                client, "Entry id %" PRId64 " does not exist", id
            );
            return;
        }

        state->id = id;
        if (id == -1)
            state->cleared = true;
        set(state, NULL);

        ipc_client_send_success(client);
    }
    else if (strcmp(type, IPC_REQ_DELETE_ENTRY) == 0)
    {
        int64_t id;

        if (!extract_json_object(req->payload, JSON_INT("id", &id), NULL))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        if (!database_id_exists(&state->db, id))
        {
            ipc_client_send_error(
                client, "Entry id %" PRId64 " does not exist", id
            );
            return;
        }

        if (!database_delete_entry(&state->db, id))
        {
            ipc_client_send_error(client, IPC_DB_ERROR);
            return;
        }

        ipc_client_send_success(client);
        ipc_event_entry_delete(&state->ipc, id);
    }
    else if (strcmp(type, IPC_REQ_PIN_ENTRY) == 0)
    {
        int64_t id;
        bool    pin;

        if (!extract_json_object(
                req->payload, JSON_INT("id", &id), JSON_BOOL("pin", &pin), NULL
            ))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        if (!database_id_exists(&state->db, id))
        {
            ipc_client_send_error(
                client, "Entry id %" PRId64 " does not exist", id
            );
            return;
        }

        if (!update(state, id, &pin))
            ipc_client_send_error(client, IPC_DB_ERROR);
        else
            ipc_client_send_success(client);
    }
    else
        ipc_client_send_error(client, IPC_INVALID_ARGS);
}

static void
help(void)
{
    // clang-format off
    printf("Usage: swayclip [OPTIONS]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -l, --logfile <path>      File to write log messages to\n");
    printf("  -c, --config <path>       File to parse config\n");
    printf("  -d, --ready               Print \"Ready\" when fully initialized\n");
    printf("  -s, --db <path>           File to place SQLite database\n");
    printf("  -d, --debug               Enable debug log messages\n");
    printf("  -h, --help                Show this help message\n");
    printf("  -v, --version             Show version\n");
    // clang-format on
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"logfile", required_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"db", required_argument, 0, 's'},
        {"ready", no_argument, 0, 'r'},
        {"debug", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int   c;
    int   idx;
    bool  init_log = false;
    char *config = NULL;
    char *db_file = NULL;
    bool  readymsg = false;

    while ((c = getopt_long(argc, argv, "l:c:s:rdhv", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'l':
            log_init(optarg);
            init_log = true;
            break;
        case 'c':
            free(config);
            config = strdup(optarg);
            break;
        case 's':
            free(db_file);
            db_file = strdup(optarg);
            break;
        case 'r':
            readymsg = true;
            break;
        case 'd':
            log_set_level(LOG_DEBUG);
            break;
        case 'h':
            help();
            return EXIT_SUCCESS;
        case 'v':
            printf("%s\n", PROJECT_VERSION);
            return EXIT_SUCCESS;
        default:
            free(config);
            free(db_file);
            return EXIT_FAILURE;
        }
    }

    if (!init_log)
        log_init(NULL);

    sigset_t block;

    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGPIPE);

    if (pthread_sigmask(SIG_BLOCK, &block, NULL) == -1)
    {
        log_errerror("Error setting signal mask");
        free(config);
        free(db_file);
        return EXIT_FAILURE;
    }

    struct state state;
    bool         ret = false;

    ret = config_init(&state.config, config);
    free(config);
    if (!ret)
    {
        free(db_file);
        return EXIT_FAILURE;
    }

    if (!eventloop_init(&state.loop))
    {
        config_uninit(&state.config);
        free(db_file);
        return EXIT_FAILURE;
    }

    ret = database_init(&state.db, db_file, &state.config);
    free(db_file);
    if (!ret)
    {
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

    struct wayland_signals wsignals = {
        .selection = {.callback = wsignal_selection, .callback_udata = &state},
        .send = {.callback = wsignal_send, .callback_udata = &state},
        .set = {.callback = wsignal_set, .callback_udata = &state},
        .can_set = {.callback = wsignal_can_set, .callback_udata = &state},
    };

    if (!wayland_init(&state.wayland, wsignals, &state.loop, &state.config))
    {
        database_uninit(&state.db);
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

    if (!ipc_init(&state.ipc, &state.loop, request_callback, &state))
    {
        wayland_uninit(&state.wayland);
        database_uninit(&state.db);
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

    int sig_fd = signalfd(-1, &block, SFD_NONBLOCK | SFD_CLOEXEC);

    if (sig_fd == -1 || !eventloop_add(
                            &state.loop,
                            sig_fd,
                            EVENT_PRIORITY_NORMAL,
                            EPOLLIN,
                            signal_callback,
                            &state.loop
                        ))
    {
        log_errerror("Error setting up signal mask");
        goto exit;
    }

    // We set the selections later when the Wayland seat is started (see
    // wayland.c)
    if (!database_get_setting(&state.db, DB_SETTING_LAST_ENTRY, 'i', &state.id))
        state.id = -1;
    state.cleared = false;

    if (readymsg)
    {
        printf("Ready\n");
        fflush(stdout);
    }

    ret = eventloop_run(&state.loop);
    log_info("Exiting...");

exit:
    if (sig_fd != -1)
    {
        (void)eventloop_del(&state.loop, sig_fd);
        close(sig_fd);
    }

    ipc_uninit(&state.ipc);
    wayland_uninit(&state.wayland);
    database_uninit(&state.db);
    config_uninit(&state.config);
    eventloop_uninit(&state.loop);

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}
