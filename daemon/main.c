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
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/signalfd.h>

#define IPC_INVALID_ARGS "Invalid arguments"
#define IPC_DB_ERROR "Database error"
#define IPC_MEM_ERROR "Memory error"

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

    // Hash of last/most recent selection event. Used to check if a new
    // selection event is the same in terms of mime types and data.
    uint8_t selection_hash[SHA256_BLOCK_SIZE];
    bool    selection_hash_init; // If "selection_hash" is initialized
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

static void
set(struct state *state, struct selection *ignore)
{
    if (database_save_setting(
            &state->db, DB_SETTING_LAST_ENTRY, SQLITE_INTEGER, state->id
        ))
        wayland_set(&state->wayland, ignore);
    else
        state->id = -1;
}

static void
read_callback(uint8_t *buf, ssize_t r, void *udata)
{
    SHA256_CTX *sha_ctx = udata;

    sha256_update(sha_ctx, (BYTE *)buf, r);
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
        return;

    if (!database_do_transaction(&state->db, DB_TRANSACTION_IMMEDIATE))
        return;

    // SHA256 hash used to represent this selection event.
    SHA256_CTX sha_ctx;
    int64_t    id = database_new_entry(&state->db);
    bool       ret = false;

    if (id == -1)
        goto exit;

    sha256_init(&sha_ctx);

    char   *mime_type;
    uint8_t data_id[SHA256_BLOCK_SIZE];

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
            .no_data = false
        };

        if (!io_read(&ctx, 3000))
            goto exit;

        close(fd);

        struct xarray_io *data = &ctx.data;

        // Check if data is bigger than configured max size. Shouldn't need to
        // worry about integer overflow, because that is checked in io_read().
        if (xarray_len_io(data) > (uint64_t)state->config.max_size)
            goto exit;

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
    }

    uint8_t sel_hash[SHA256_BLOCK_SIZE];

    sha256_final(&sha_ctx, sel_hash);

    // Check if selection event is the same as the prior selection event. If
    // so, then ignore it.
    if (state->selection_hash_init &&
        memcmp(sel_hash, state->selection_hash, SHA256_BLOCK_SIZE) == 0)
    {
        ret = false;
    }
    else
    {
        memcpy(state->selection_hash, sel_hash, SHA256_BLOCK_SIZE);
        (void)database_save_setting(
            &state->db,
            DB_SETTING_SELECTION_HASH,
            SQLITE_BLOB,
            sel_hash,
            SHA256_BLOCK_SIZE
        );
        state->selection_hash_init = true;
        ret = true;
    }

exit:
    database_do_transaction(
        &state->db, ret ? DB_TRANSACTION_COMMIT : DB_TRANSACTION_ROLLBACK
    );

    if (ret)
    {
        state->id = id;
        // Don't want to set the source selection, let the original source
        // client be.
        set(state, sel);
    }
    else
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
    ;

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

    // Maybe run this in the event loop (non-blocking)? Not sure if worth it...
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
        JSON_INT("creation-time", creation_time),
        JSON_INT("update-time", update_time),
        JSON_BOOL("pinned", pinned),
        JSON_OBJ("mime-types", mime_types),
        NULL
    );

    if (json_object_array_add(arr, obj) == -1)
        json_object_put(obj);
}

static void
request_callback(
    struct ipc_client *client, struct ipc_message *req, void *udata
)
{
    struct state *state = udata;

    const char *type;

    if (!extract_json_object(req->payload, JSON_STR("type", &type), NULL))
    {
        ipc_client_send_error(client, IPC_INVALID_ARGS);
        return;
    }

    if (strcmp(type, IPC_REQ_SUBSCRIBE) == 0)
    {
        struct json_object *arr =
            json_object_object_get(req->payload, "events");

        if (arr == NULL)
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
            else if (strcmp(event, IPC_EVENT_CLIPBOARD_STATE) == 0)
                events |= IPC_EVENT_FLAG_CLIPBOARD_STATE;
            else
            {
                ipc_client_send_error(client, "Unknown event \"%s\"", event);
                return;
            }
        }

        ipc_client_set_events(client, events);
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
                JSON_STR("mime-type", &mime_type),
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

        ipc_client_send(client, json_object_new_object(), fd);
    }
    else
        ipc_client_send_error(client, IPC_INVALID_ARGS);
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"logfile", required_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"db", required_argument, 0, 's'},
        {"ready", required_argument, 0, 'r'},
        {"debug", no_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int   c;
    int   idx;
    bool  init_log = false;
    char *config = NULL;
    char *data_dir = NULL;
    bool readymsg = false;

    while ((c = getopt_long(argc, argv, "l:c:s:rdv", options, &idx)) != -1)
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
            free(data_dir);
            data_dir = strdup(optarg);
            break;
        case 'r':
            readymsg = true;
            break;
        case 'd':
            log_set_level(LOG_DEBUG);
            break;
        case 'v':
            printf("%s\n", PROJECT_VERSION);
            break;
        default:
            free(config);
            free(data_dir);
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
        free(data_dir);
        return EXIT_FAILURE;
    }

    struct state state;
    bool         ret = false;

    ret = config_init(&state.config, config);
    free(config);
    if (!ret)
    {
        free(data_dir);
        return EXIT_FAILURE;
    }

    if (!eventloop_init(&state.loop))
    {
        config_uninit(&state.config);
        free(data_dir);
        return EXIT_FAILURE;
    }

    struct wayland_signals wsignals = {
        .selection = {.callback = wsignal_selection, .callback_udata = &state},
        .send = {.callback = wsignal_send, .callback_udata = &state},
        .set = {.callback = wsignal_set, .callback_udata = &state},
        .can_set = {.callback = wsignal_can_set, .callback_udata = &state}
    };

    ret = database_init(&state.db, data_dir, &state.config);
    free(data_dir);
    if (!ret)
    {
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

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

    state.selection_hash_init = database_get_setting(
        &state.db,
        DB_SETTING_SELECTION_HASH,
        SQLITE_BLOB,
        state.selection_hash,
        SHA256_BLOCK_SIZE,
        NULL
    );
    if (!database_get_setting(
            &state.db, DB_SETTING_LAST_ENTRY, SQLITE_INTEGER, &state.id
        ))
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
