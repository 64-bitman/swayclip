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

#include "swayclip-list.h"
#include "common/json_util.h"
#include <json.h>

struct _SwayclipEntry
{
    GObject parent;

    SwayclipList       *list; // NULL if entry was deleted
    SwayclipConnection *ct;

    guint   pos;
    int64_t id; // -1 if not loaded yet

    int64_t creation_time;
    int64_t update_time;

    gboolean pinned;
    gboolean current;

    // Key is mime type and value is GBytes (or NULL if not loaded yet).
    GHashTable *mime_types;

    SwayclipContentType content;
    char               *content_mime;
};

typedef enum
{
    ENTRY_PROP_0,
    ENTRY_PROP_POSITION,
    ENTRY_PROP_ID,
    ENTRY_PROP_CREATION_TIME,
    ENTRY_PROP_UPDATE_TIME,
    ENTRY_PROP_PINNED,
    ENTRY_PROP_CURRENT,
    N_ENTRY_PROPS
} SwayclipEntryProp;

static GParamSpec *entry_props[N_ENTRY_PROPS] = {NULL};

G_DEFINE_TYPE(SwayclipEntry, swayclip_entry, G_TYPE_OBJECT)

static void
swayclip_entry_finalize(GObject *obj)
{
    SwayclipEntry *self = SWAYCLIP_ENTRY(obj);

    g_hash_table_unref(self->mime_types);
    g_free(self->content_mime);

    G_OBJECT_CLASS(swayclip_entry_parent_class)->finalize(obj);
}

static void
swayclip_entry_dispose(GObject *obj)
{
    SwayclipEntry *self = SWAYCLIP_ENTRY(obj);

    g_clear_object(&self->list);
    g_clear_object(&self->ct);

    G_OBJECT_CLASS(swayclip_entry_parent_class)->dispose(obj);
}

static void
swayclip_entry_class_init(SwayclipEntryClass *class)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(class);

    obj_class->finalize = swayclip_entry_finalize;
    obj_class->dispose = swayclip_entry_dispose;

    entry_props[ENTRY_PROP_POSITION] = g_param_spec_uint(
        "position",
        NULL,
        NULL,
        0,
        G_MAXUINT,
        0,
        G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
    entry_props[ENTRY_PROP_ID] = g_param_spec_int64(
        "id",
        NULL,
        NULL,
        -1,
        G_MAXINT64,
        -1,
        G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
    entry_props[ENTRY_PROP_CREATION_TIME] = g_param_spec_int64(
        "creation-time",
        NULL,
        NULL,
        0,
        G_MAXINT64,
        0,
        G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
    entry_props[ENTRY_PROP_UPDATE_TIME] = g_param_spec_int64(
        "update-time",
        NULL,
        NULL,
        0,
        G_MAXINT64,
        0,
        G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
    entry_props[ENTRY_PROP_PINNED] = g_param_spec_boolean(
        "pinned", NULL, NULL, FALSE, G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
    entry_props[ENTRY_PROP_CURRENT] = g_param_spec_boolean(
        "current", NULL, NULL, FALSE, G_PARAM_STATIC_STRINGS | G_PARAM_READABLE
    );
}

static void
swayclip_entry_init(SwayclipEntry *self)
{
    self->id = -1;

    self->mime_types = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
    );

    self->content = SWAYCLIP_CONTENT_UNKNOWN;
}

static void
load_entry_cb(
    SwayclipConnection *ct, GAsyncResult *result, SwayclipEntry *entry
)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(SwayclipMessage) resp =
        swayclip_connection_request_finish(ct, result, &error);

    if (resp == NULL)
    {
        g_warning("Error loading entry: %s", error->message);
        goto exit;
    }

exit:
    g_object_unref(entry);
}

static SwayclipEntry *
swayclip_entry_new(SwayclipList *list, SwayclipConnection *ct, guint pos)
{
    SwayclipEntry *entry = g_object_new(SWAYCLIP_TYPE_ENTRY, NULL);

    entry->list = g_object_ref(list);
    entry->ct = g_object_ref(ct);
    entry->pos = pos;

    swayclip_connection_request(
        ct,
        build_json_object(
            NULL,
            -1,
            JSON_FIELD_STR("type", "get_history"),
            JSON_FIELD_INT("start", pos),
            JSON_FIELD_INT("n", 1),
            NULL
        ),
        -1,
        G_PRIORITY_DEFAULT,
        NULL,
        (GAsyncReadyCallback)load_entry_cb,
        g_object_ref(entry)
    );

    return entry;
}

struct _SwayclipList
{
    GObject parent;

    SwayclipConnection *ct;

    guint history_length;

    // Sorted in ascending order
    GPtrArray *entries;
};

static void
swayclip_entry_delete(void *udata G_GNUC_UNUSED, SwayclipEntry *self)
{
    if (self->list == NULL)
        return;

    g_ptr_array_remove(self->list->entries, self);
    g_clear_object(&self->list);
}

static void *
swayclip_list_model_get_item(GListModel *list, guint pos)
{
    SwayclipList *self = SWAYCLIP_LIST(list);

    for (guint i = 0; i < self->entries->len; i++)
    {
        SwayclipEntry *entry = self->entries->pdata[i];

        if (entry->pos == pos)
            return g_object_ref(entry);
    }

    // Create new entry
    SwayclipEntry *entry = swayclip_entry_new(self, self->ct, pos);

    guint idx = 0;

    for (guint i = 0; i < self->entries->len; i++)
    {
        SwayclipEntry *entry = self->entries->pdata[i];

        if (entry->pos > pos)
        {
            idx = i;
            break;
        }
    }

    g_ptr_array_insert(self->entries, idx, entry);
    g_object_weak_ref(
        G_OBJECT(entry), (GWeakNotify)swayclip_entry_delete, NULL
    );

    return entry;
}

static GType
swayclip_list_model_get_item_type(GListModel *list G_GNUC_UNUSED)
{
    return SWAYCLIP_TYPE_ENTRY;
}

static guint
swayclip_list_model_get_n_items(GListModel *list)
{
    SwayclipList *self = SWAYCLIP_LIST(list);

    return self->entries->len;
}

static void
swayclip_list_model_init(GListModelInterface *iface)
{
    iface->get_item = swayclip_list_model_get_item;
    iface->get_item_type = swayclip_list_model_get_item_type;
    iface->get_n_items = swayclip_list_model_get_n_items;
}

// clang-format off
G_DEFINE_TYPE_WITH_CODE(SwayclipList, swayclip_list, G_TYPE_OBJECT, G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, swayclip_list_model_init))
// clang-format on

static void
swayclip_list_finalize(GObject *obj)
{
    SwayclipList *self = SWAYCLIP_LIST(obj);

    g_ptr_array_unref(self->entries);

    G_OBJECT_CLASS(swayclip_list_parent_class)->finalize(obj);
}

static void
swayclip_list_dispose(GObject *obj)
{
    SwayclipList *self = SWAYCLIP_LIST(obj);

    g_clear_object(&self->ct);

    G_OBJECT_CLASS(swayclip_list_parent_class)->dispose(obj);
}

static void
swayclip_list_class_init(SwayclipListClass *class)
{
    GObjectClass *obj_class = G_OBJECT_CLASS(class);

    obj_class->finalize = swayclip_list_finalize;
    obj_class->dispose = swayclip_list_dispose;
}

static void
swayclip_list_init(SwayclipList *self)
{
    self->entries = g_ptr_array_new_full(200, NULL);
}

static void
history_length_cb(
    SwayclipConnection *ct, GAsyncResult *result, SwayclipList *list
)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(SwayclipMessage) resp =
        swayclip_connection_request_finish(ct, result, &error);

    if (resp == NULL)
    {
        g_warning("Error querying history length: %s", error->message);
        goto exit;
    }

    int64_t size;

    if (!extract_json_object(resp->obj, JSON_EXTRACT_INT("size", &size), NULL))
        goto exit;

    list->history_length = MAX(size, G_MAXUINT);

exit:
    g_object_unref(list);
}

SwayclipList *
swayclip_list_new(SwayclipConnection *ct)
{
    SwayclipList *list = g_object_new(SWAYCLIP_TYPE_LIST, NULL);

    list->ct = g_object_ref(ct);

    // Get initial history length and subscribe to relevant events in one go
    swayclip_connection_request(
        ct,
        build_json_array(
            NULL,
            build_json_object(
                NULL, -1, JSON_FIELD_STR("type", "get_history_length"), NULL
            ),
            build_json_object(
                NULL,
                -1,
                JSON_FIELD_STR("type", "subscribe"),
                JSON_FIELD_ARRAY(
                    "events",
                    build_json_array(
                        NULL,
                        JSON_VAL_STR("entry_add"),
                        JSON_VAL_STR("entry_delete"),
                        JSON_VAL_STR("entry_update"),
                        JSON_VAL_STR("entry_move"),
                        JSON_VAL_STR("entry_state"),
                        NULL
                    )
                ),
                NULL
            ),
            NULL
        ),
        -1,
        G_PRIORITY_DEFAULT,
        NULL,
        (GAsyncReadyCallback)history_length_cb,
        g_object_ref(list)
    );

    return list;
}
