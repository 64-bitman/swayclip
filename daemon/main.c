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

struct state;

xlist_declare(send);
struct send_context
{
    int           fd;
    struct state *state;
    sqlite3_blob *blob; // If NULL then entry is transient

    uint8_t  buf[4096];
    uint8_t *ptr;       // Pointer to write from
    uint32_t remaining; // Remaining bytes in "buf" (or "ptr" if transient)

    int sz;  // Blob size
    int off; // Offset to read from, unused if entry is transient

    struct xlist_send link;
};
xlist_define(send, struct send_context, link);

struct state
{
    struct eventloop loop;

    struct config config;

    struct wayland  wayland;
    struct database db;
    struct ipc      ipc;

    uint8_t buf[4096]; // Used for I/O operations

    // ID of current entry that all enabled selections are synced to. -1 if no
    // entry set, or if "cleared" is true, then clipboard is cleared. If 0, then
    // the current entry is transient.
    int64_t id;
    bool    cleared;

    struct xlist_send send_contexts;

    // Contains data for transient entry
    struct
    {
        struct
        {
            char    *name;
            uint8_t *data;
            uint32_t sz;
        }       *mime_types;
        uint32_t n_mime_types;
    } transient;
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

/*
 * Note that this does not update "update_time"
 */
static void
set(struct state *state, struct selection *ignore, int64_t id)
{
    if (state->id > 0)
        ipc_event_entry_state(&state->ipc, state->id, false);

    state->id = id;
    if (state->id == -1)
        state->cleared = true;

    if (state->id > 0)
        (void)database_save_setting(
            &state->db, DB_SETTING_LAST_ENTRY, 'i', state->id
        );
    wayland_set(&state->wayland, ignore);

    if (state->id > 0)
        ipc_event_entry_state(&state->ipc, state->id, true);
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
trim_callback(int64_t id, void *udata)
{
    struct state *state = udata;

    ipc_event_entry_delete(&state->ipc, id);
}

static void
clear_transient(struct state *state)
{
    for (uint32_t i = 0; i < state->transient.n_mime_types; i++)
    {
        free(state->transient.mime_types[i].name);
        free(state->transient.mime_types[i].data);
    }
    free(state->transient.mime_types);
    state->transient.mime_types = NULL;
    state->transient.n_mime_types = 0;
}

static bool
receive_data(
    struct state                     *state,
    struct io_read                   *ctx,
    struct ext_data_control_offer_v1 *offer,
    char                             *mime_type
)
{
    int fd = wayland_get_offer_fd(&state->wayland, offer, mime_type);

    if (fd == -1)
        return false;

    if (!set_fd_nonblocking(fd))
    {
        close(fd);
        return false;
    }

    ctx->fd = fd;
    if (!io_read(ctx, 3000, state->config.max_size))
    {
        close(fd);
        return false;
    }
    close(fd);

    return true;
}

static void
wsignal_selection(
    struct selection                 *sel,
    struct ext_data_control_offer_v1 *offer,
    struct xarray_mime_type          *mime_types,
    bool                              transient,
    void                             *udata
)
{
    struct state *state = udata;

    if (xarray_len_mime_type(mime_types) == 0)
    {
        log_debug("Selection event has no allowed mime types, ignoring");
        return;
    }

    clear_transient(state);

    if (transient)
    {
        log_debug("Entry is transient");
        state->id = -1;

        // Receive mime types transiently
        state->transient.mime_types = calloc(
            xarray_len_mime_type(mime_types),
            sizeof(*state->transient.mime_types)
        );

        if (state->transient.mime_types == NULL)
        {
            log_errerror("Error allocating array for transient entry");
            return;
        }
        state->transient.n_mime_types = xarray_len_mime_type(mime_types);

        char    *mime_type;
        uint32_t i = 0;

        xarray_foreach_val(mime_type, mime_types, mime_type)
        {
            struct io_read ctx = {
                .buf = state->buf,
                .bufsize = sizeof(state->buf),
                .data_callback = NULL
            };

            if (!receive_data(state, &ctx, offer, mime_type))
            {
                clear_transient(state);
                return;
            }

            state->transient.mime_types[i].name = strdup(mime_type);
            if (state->transient.mime_types[i].name == NULL)
            {
                xarray_uninit_io(&ctx.data);
                clear_transient(state);
                return;
            }

            state->transient.mime_types[i].data =
                xarray_steal_io(&ctx.data, &state->transient.mime_types[i].sz);
            i++;
        }

        set(state, sel, 0);

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
        SHA256_CTX data_sha_ctx;

        sha256_init(&data_sha_ctx);

        struct io_read ctx = {
            .buf = state->buf,
            .bufsize = sizeof(state->buf),
            .data_callback = read_callback,
            .callback_udata = &data_sha_ctx,
        };

        if (!receive_data(state, &ctx, offer, mime_type))
            goto exit;

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
        if (ret)
            ret = update(state, id, NULL);
        ignore = true;
    }

    if (ret)
    {
        if (moved)
        {
            ipc_event_entry_move(&state->ipc, id, old_pos, 0);
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
        // Do not become the source client for the selection that the event came
        // from.
        set(state, sel, id);
    }
    else if (!ignore)
        state->id = -1;

    (void)database_trim(&state->db, trim_callback, state);
}

static void
send_context_free(struct send_context *ctx)
{
    (void)eventloop_del(&ctx->state->loop, ctx->fd);
    close(ctx->fd);
    if (ctx->blob != NULL)
        sqlite3_blob_close(ctx->blob);
    xlist_unlink_send(ctx);
    free(ctx);
}

static bool
write_callback(int fd, int events, void *udata)
{
    struct send_context *ctx = udata;

    if (events & (EPOLLHUP | EPOLLERR))
        goto end;

    // Keep writing until we get EAGAIN or EWOULDBLOCK
    while (true)
    {
        if (ctx->blob != NULL && ctx->remaining == 0)
        {
            int len = MIN((size_t)ctx->sz - ctx->off, sizeof(ctx->buf));

            if (len == 0)
                goto end;

            if (sqlite3_blob_read(ctx->blob, ctx->buf, len, ctx->off) !=
                SQLITE_OK)
            {
                log_error(
                    "Error reading from blob: %s",
                    sqlite3_errmsg(ctx->state->db.handle)
                );
                goto end;
            }
            ctx->ptr = ctx->buf;
            ctx->remaining = len;
            ctx->off += len;
        }

        ssize_t w;

        while (true)
        {
            w = write(fd, ctx->ptr, ctx->remaining);

            if (w == -1)
            {
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    // Poll until writable again
                    return false;
                log_errerror("Error writing to fd %d", fd);
                goto end;
            }
            assert(w != 0);
            break;
        }

        ctx->ptr += w;
        ctx->remaining -= w;
        if (ctx->blob == NULL && ctx->remaining == 0)
            goto end;
    }

end:
    send_context_free(ctx);
    return true;
}

static void
wsignal_send(const char *mime_type, int fd, void *udata)
{
    struct state *state = udata;

    if (state->id == -1)
        goto fail;
    if (!set_fd_nonblocking(fd))
        goto fail;

    struct send_context *ctx = malloc(sizeof(*ctx));

    if (ctx == NULL)
        goto fail;

    sqlite3_blob *blob = NULL;

    if (state->id > 0)
    {
        blob = database_get_data(&state->db, state->id, mime_type);

        if (blob == NULL)
        {
            free(ctx);
            goto fail;
        }
    }

    ctx->fd = fd;
    ctx->state = state;
    ctx->blob = blob;

    if (blob == NULL)
    {
        // Not sure if this can happen but still check
        if (state->transient.n_mime_types == 0)
            goto fail;

        ctx->ptr = NULL;

        for (uint32_t i = 0; i < state->transient.n_mime_types; i++)
            if (strcmp(state->transient.mime_types[i].name, mime_type) == 0)
            {
                ctx->remaining = state->transient.mime_types[i].sz;
                ctx->ptr = state->transient.mime_types[i].data;
                break;
            }

        if (ctx->ptr == NULL)
        {
            free(ctx);
            goto fail;
        }
    }
    else
    {
        ctx->ptr = ctx->buf;
        ctx->remaining = 0;
        ctx->sz = sqlite3_blob_bytes(blob);
        ctx->off = 0;
    }

    bool res = eventloop_add(
        &state->loop, fd, EVENT_PRIORITY_NORMAL, EPOLLOUT, write_callback, ctx
    );

    if (!res)
    {
        free(ctx);
        if (blob != NULL)
            sqlite3_blob_close(blob);
        goto fail;
    }

    xlist_insert_after_send(&state->send_contexts, ctx);

    return;
fail:
    close(fd);
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

    if (state->id >= 0)
    {
        source = ext_data_control_manager_v1_create_data_source(mgr);
        if (state->id == 0)
        {
            // Transient entry, directly add the mime types
            for (uint32_t i = 0; i < state->transient.n_mime_types; i++)
                ext_data_control_source_v1_offer(
                    source, state->transient.mime_types[i].name
                );
        }
        else
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
    struct state       *state = ((void **)udata)[1];
    struct database    *db = &state->db;
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
        JSON_BOOL("current", state->id == id),
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
            else if (strcmp(event, IPC_EVENT_ENTRY_STATE) == 0)
                events |= IPC_EVENT_FLAG_ENTRY_STATE;
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
    else if (strcmp(type, IPC_REQ_GET_HISTORY) == 0)
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

        const void *udata[] = {arr, state};

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

        set(state, NULL, id);
        if (update(state, id, NULL))
            ipc_client_send_success(client);
        else
            ipc_client_send_error(client, IPC_DB_ERROR);
    }
    else if (strcmp(type, IPC_REQ_DELETE_ENTRY) == 0)
    {
        int64_t id;

        if (!extract_json_object(req->payload, JSON_INT("id", &id), NULL))
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        // If "id" is -1, then clear clipboard history
        if (id == -1)
        {
            if (!database_clear(&state->db))
            {
                ipc_client_send_error(client, IPC_DB_ERROR);
                return;
            }
        }
        else
        {
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
        }

        // If deleted entry was set as the clipboard, then clear the clipboard
        if (id == -1 || id == state->id)
        {
            // Don't want to send "entry_state" event for the now deleted
            // entry.
            state->id = -1;
            set(state, NULL, -1);
        }

        ipc_client_send_success(client);
        ipc_event_entry_delete(&state->ipc, id);
    }
    else if (strcmp(type, IPC_REQ_PIN_ENTRY) == 0)
    {
        int64_t     id;
        const char *pinstr;

        if (!extract_json_object(
                req->payload,
                JSON_INT("id", &id),
                JSON_STR("pin", &pinstr),
                NULL
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

        bool pin;

        if (strcmp(pinstr, "yes") == 0)
            pin = true;
        else if (strcmp(pinstr, "no") == 0)
            pin = false;
        else if (strcmp(pinstr, "toggle") == 0)
            pin = !database_entry_is_pinned(&state->db, id);
        else
        {
            ipc_client_send_error(client, IPC_INVALID_ARGS);
            return;
        }

        if (update(state, id, &pin))
            ipc_client_send_success(client);
        else
            ipc_client_send_error(client, IPC_DB_ERROR);
    }
    else if (strcmp(type, IPC_REQ_GET_CURRENT) == 0)
    {
        ipc_client_send(client, json_object_new_int64(state->id), -1);
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
    printf("  -l, --logfile <PATH>      File to write log messages to.\n");
    printf("  -c, --config <PATH>       File to parse config.\n");
    printf("  -s, --db <PATH>           File to place SQLite database.\n");
    printf("  -r, --ready               Print \"Ready\" when fully initialized.\n");
    printf("  -f, --fatal               Make log warnings and errors fatal.\n");
    printf("  -d, --debug               Enable debug log messages.\n");
    printf("  -h, --help                Show this help message.\n");
    printf("  -v, --version             Show version.\n");
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
        {"fatal", no_argument, 0, 'f'},
        {"debug", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int   c;
    int   idx;
    char *config = NULL;
    char *db_file = NULL;
    bool  readymsg = false;
    bool fatal = false;
    char *logfile = NULL;
    bool loginit = false;

    while ((c = getopt_long(argc, argv, "l:c:s:rfdhv", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'l':
            free(logfile);
            logfile = strdup(optarg);
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
        case 'f':
            fatal = true;
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

    if (logfile != NULL)
        loginit = true;
    log_init(logfile, fatal);
    free(logfile);

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

    if (!loginit && state.config.logfile != NULL)
        log_init(state.config.logfile, fatal);

    if (!eventloop_init(&state.loop))
    {
        config_uninit(&state.config);
        free(db_file);
        return EXIT_FAILURE;
    }

    if (db_file == NULL && state.config.db != NULL)
        db_file = strdup(state.config.db);
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
    // wayland.c). Dont use last entry is "set_on_startup" config opt is false.
    if (!state.config.set_on_startup ||
        !database_get_setting(&state.db, DB_SETTING_LAST_ENTRY, 'i', &state.id))
        state.id = -1;
    state.cleared = false;

    if (readymsg)
    {
        printf("Ready\n");
        fflush(stdout);
    }

    xlist_init_send(&state.send_contexts);

    ret = eventloop_run(&state.loop);
    log_info("Exiting...");

    struct send_context *ctx;
    xlist_foreach_safe(send, &state.send_contexts, ctx)
    {
        send_context_free(ctx);
    }

    clear_transient(&state);

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
