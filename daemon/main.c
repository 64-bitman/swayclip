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
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signalfd.h>

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
    const struct sc_array_astr       *mime_types,
    void                             *udata
)
{
    struct state *state = udata;

    if (sc_array_size(mime_types) == 0)
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

    const char *mime_type;
    uint8_t     data_id[SHA256_BLOCK_SIZE];

    sc_array_foreach(mime_types, mime_type)
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

        struct sc_buf *data = &ctx.data;

        // Check if data is bigger than configured max size. Shouldn't need to
        // worry about integer overflow, because that is checked in io_read().
        if (sc_buf_size(data) > (uint64_t)state->config.max_size)
            goto exit;

        sha256_final(&data_sha_ctx, data_id);

        sha256_update(&sha_ctx, (BYTE *)mime_type, strlen(mime_type));
        sha256_update(&sha_ctx, data_id, SHA256_BLOCK_SIZE);

        bool ret = database_new_mime_type(
            &state->db, id, mime_type, data_id, data->mem, sc_buf_size(data)
        );

        sc_buf_term(data);
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

    (void)io_write(&ctx, 3000);

    sqlite3_blob_close(blob);
}

static void
mime_type_callback(const char *mime_type, void *udata)
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
            &state->db, state->id, mime_type_callback, source
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
request_callback(
    struct ipc_client *client, struct ipc_message *req, void *udata
)
{
    struct state *state = udata;

    switch (req->type)
    {
    case IPC_MESSAGE_GET_HISTORY_SIZE:
    {
        int64_t size = database_get_history_size(&state->db);

        ipc_client_add_object(
            client,
            build_json_object(JUTIL_FLAGS, IPC_SUCCESS, "size", 'i', size, NULL)
        );
        break;
    }
    case IPC_MESSAGE_SUBSCRIBE:
    {
        break;
    }
    case IPC_MESSAGE_GET_ENTRY:
    {
        break;
    }
    default:
        break;
    }
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"logfile", required_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"db", required_argument, 0, 's'},
        {"debug", no_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int   c;
    int   idx;
    bool  init_log = false;
    char *config = NULL;
    char *data_dir = NULL;

    while ((c = getopt_long(argc, argv, "l:c:s:dv", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'l':
            log_init(optarg);
            init_log = true;
            break;
        case 'c':
            config = strdup(optarg);
            break;
        case 's':
            data_dir = strdup(optarg);
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
