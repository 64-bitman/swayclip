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

#include "wayland.h"
#include "common/log.h"
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct seat;

struct selection
{
    enum wayland_selection_type type;

    struct seat *seat;
    bool         enabled;

    // We need to store the data offer so that we can verify NULL selection
    // events. Probably could also do another way, but ehh...
    struct ext_data_control_offer_v1  *ext_data_offer;
    struct ext_data_control_source_v1 *ext_data_source;

    // Timer used to check if NULL selection event is valid. -1 if not set
    int null_timerfd;
};

struct seat
{
    struct wayland *wayland;

    bool            enabled;
    struct wl_seat *proxy;
    char           *name; // May be NULL
    uint32_t        global_name;

    struct ext_data_control_device_v1 *ext_data_device;

    struct selection sel_regular;
    struct selection sel_primary;

    struct xarray_mime_type mime_types;
    bool                    blocked;
    bool                    transient;

    struct xlist_seat link;
};
xlist_define(seat, struct seat, link);

struct toplevel
{
    struct wayland *wayland;

    struct zwlr_foreign_toplevel_handle_v1 *proxy;

    char *app_id; // May be NULL
    char *title;  // May be NULL
    bool  activated;

    struct config_toplevel *config;
    bool                    ack;

    struct xlist_toplevel link;
};
xlist_define(toplevel, struct toplevel, link);

static void wayland_del_seat(struct seat *seat);

static void
selection_init(struct selection *sel, struct seat *seat)
{
    if (sel == &seat->sel_regular)
        sel->type = WAYLAND_SELECTION_REGULAR;
    else
        sel->type = WAYLAND_SELECTION_PRIMARY;

    sel->seat = seat;
    sel->null_timerfd = -1;
}

static void
selection_uninit(struct selection *sel)
{
    if (sel->ext_data_offer != NULL)
    {
        ext_data_control_offer_v1_destroy(sel->ext_data_offer);
        sel->ext_data_offer = NULL;
    }
    if (sel->ext_data_source != NULL)
    {
        ext_data_control_source_v1_destroy(sel->ext_data_source);
        sel->ext_data_source = NULL;
    }

    if (sel->null_timerfd != -1)
    {
        eventloop_del(sel->seat->wayland->wct.loop, sel->null_timerfd);
        close(sel->null_timerfd);
        sel->null_timerfd = -1;
    }
}

static void
data_source_event_send(
    void                                    *udata,
    struct ext_data_control_source_v1 *proxy UNUSED,
    const char                              *mime_type,
    int                                      fd
)
{
    struct selection *sel = udata;
    struct wayland   *wayland = sel->seat->wayland;

    wayland->signals.send.callback(
        mime_type, fd, wayland->signals.send.callback_udata
    );
}

static void
data_source_event_cancelled(
    void *udata, struct ext_data_control_source_v1 *proxy
)
{
    struct selection *sel = udata;

    if (sel->ext_data_source == proxy)
        sel->ext_data_source = NULL;
    ext_data_control_source_v1_destroy(proxy);
}

static const struct ext_data_control_source_v1_listener data_source_listener = {
    .send = data_source_event_send, .cancelled = data_source_event_cancelled
};

static const char *
selection_str(struct selection *sel)
{
    return sel->type == WAYLAND_SELECTION_REGULAR ? "regular" : "primary";
}

static void
selection_set(struct selection *sel)
{
    struct wayland *wayland = sel->seat->wayland;
    bool            clear = false;

    struct ext_data_control_source_v1 *source = wayland->signals.set.callback(
        wayland->ext_data_mgr, &clear, wayland->signals.set.callback_udata
    );

    if (!clear && source == NULL)
        return;

    log_debug(
        "Setting %s selection for seat \"%s\" to %p",
        selection_str(sel),
        sel->seat->name,
        source
    );

    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);
    sel->ext_data_source = source;

    if (source != NULL)
        ext_data_control_source_v1_add_listener(
            source, &data_source_listener, sel
        );

    switch (sel->type)
    {
    case WAYLAND_SELECTION_REGULAR:
        ext_data_control_device_v1_set_selection(
            sel->seat->ext_data_device, source
        );
        break;
    case WAYLAND_SELECTION_PRIMARY:
        ext_data_control_device_v1_set_primary_selection(
            sel->seat->ext_data_device, source
        );
        break;
    }
}

/*
 * Check if "target" matches any of the regexes in arr.
 */
bool
match_regex_array(struct xarray_regex *arr, const char *target)
{
    regex_t *reg;

    xarray_foreach(regex, arr, reg)
    {
        if (regexec(reg, target, 0, NULL, 0) == 0)
            return true;
    }
    return false;
}

static bool
seat_check_mime_type(
    struct seat         *seat,
    const char          *mime_type,
    struct xarray_regex *blocked,
    struct xarray_regex *allowed,
    struct xarray_regex *transient
)
{
    if (seat->blocked)
        return false;

    // Do not save entry if mime type is configured to be blocked.
    if (xarray_len_regex(blocked) > 0 && match_regex_array(blocked, mime_type))
    {
        seat->blocked = true;
        return false;
    }

    if (!seat->transient && match_regex_array(transient, mime_type))
        seat->transient = true;

    // Check if mime type is allowed to be saved
    if (xarray_len_regex(allowed) > 0 && !match_regex_array(allowed, mime_type))
        return false;

    return true;
}

static void
data_offer_event_offer(
    void                                   *udata,
    struct ext_data_control_offer_v1 *proxy UNUSED,
    const char                             *mime_type
)
{
    struct seat   *seat = udata;
    struct config *config = seat->wayland->config;

    if (!seat_check_mime_type(
            seat,
            mime_type,
            &config->blocked_mime_types,
            &config->allowed_mime_types,
            &config->transient_mime_types
        ))
        return;

    if (seat->wayland->active_toplevel != NULL)
    {
        struct toplevel        *tp = seat->wayland->active_toplevel;
        struct config_toplevel *config_tp = tp->config;

        assert(config_tp != NULL);
        if (!seat_check_mime_type(
                seat,
                mime_type,
                &config_tp->blocked_mime_types,
                &config_tp->allowed_mime_types,
                &config_tp->transient_mime_types
            ))
            return;
    }

    char *str = strdup(mime_type);

    if (str == NULL)
        return;
    xarray_add_mime_type(&seat->mime_types, str);
}

static const struct ext_data_control_offer_v1_listener data_offer_listener = {
    .offer = data_offer_event_offer
};

static void
seat_clear_mime_types(struct seat *seat)
{
    char *mime_type;
    xarray_foreach_val(mime_type, &seat->mime_types, mime_type) free(mime_type);
    xarray_uninit_mime_type(&seat->mime_types);
}

static void
data_device_event_data_offer(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    struct seat *seat = udata;

    seat_clear_mime_types(seat);
    seat->blocked = false;
    seat->transient = false;

    ext_data_control_offer_v1_add_listener(
        offer_proxy, &data_offer_listener, seat
    );
}

static bool
null_timer_callback(int fd, int events, void *udata)
{
    struct selection *sel = udata;

    if (events & (EPOLLHUP | EPOLLERR))
    {
        log_errwarn("Error polling timer fd for null check");
        goto stop;
    }
    if (!(events & EPOLLIN))
        return false;

    uint64_t exp;
    ssize_t  r = read(fd, &exp, sizeof(exp));

    if (r == -1)
    {
        log_errwarn("Error reading timer fd for null check");
        goto stop;
    }

    if (sel->ext_data_offer == NULL)
        // NULL selection event is valid, become the source client
        selection_set(sel);

stop:
    sel->null_timerfd = -1;
    close(fd);
    return true;
}

static void
selection_event(
    struct seat                      *seat,
    struct ext_data_control_offer_v1 *offer,
    struct selection                 *sel
)
{
    struct wayland_signals *signals = &seat->wayland->signals;

    if (!sel->enabled)
    {
        if (offer != NULL)
            ext_data_control_offer_v1_destroy(offer);
        return;
    }

    log_debug(
        "New selection event for \"%s\" %s selection: %p",
        seat->name,
        selection_str(sel),
        offer
    );

    if (sel->ext_data_offer != NULL)
        ext_data_control_offer_v1_destroy(sel->ext_data_offer);

    if (sel->ext_data_source != NULL || seat->blocked)
    {
        if (seat->blocked)
            log_debug("Selection event blocked");
        else
            log_debug("Currently source client, ignoring");
        // Currently source client or blocked, ignore
        if (offer != NULL)
            ext_data_control_offer_v1_destroy(offer);
        sel->ext_data_offer = NULL;
        return;
    }

    sel->ext_data_offer = offer;

    if (offer == NULL)
    {
        struct wayland *wayland = seat->wayland;

        // If there is nothing to be set or timer is already set, don't add a
        // timer
        if (sel->null_timerfd != -1 ||
            !wayland->signals.can_set.callback(
                wayland->signals.can_set.callback_udata
            ))
            return;

        int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

        if (fd == -1)
        {
            log_errerror("Error creating timer fd for null check");
            return;
        }

        struct itimerspec spec = {0};

        // Set delay to 1 ms
        spec.it_value.tv_nsec = 1000000;

        if (timerfd_settime(fd, 0, &spec, NULL) == -1)
        {
            log_errerror("Error setting timer fd for null check");
            close(fd);
            return;
        }

        if (!eventloop_add(
                wayland->wct.loop,
                fd,
                EVENT_PRIORITY_NORMAL,
                EPOLLIN,
                null_timer_callback,
                sel
            ))
            close(fd);
        else
            sel->null_timerfd = fd;
        return;
    }

    signals->selection.callback(
        sel,
        offer,
        &seat->mime_types,
        seat->transient,
        signals->selection.callback_udata
    );
    seat_clear_mime_types(seat);
}

static void
data_device_event_selection(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    struct seat *seat = udata;
    selection_event(seat, offer_proxy, &seat->sel_regular);
}

static void
data_device_event_primary_selection(
    void                                    *udata,
    struct ext_data_control_device_v1 *proxy UNUSED,
    struct ext_data_control_offer_v1        *offer_proxy
)
{
    struct seat *seat = udata;
    selection_event(seat, offer_proxy, &seat->sel_primary);
}

static void
data_device_event_finished(
    void *udata, struct ext_data_control_device_v1 *proxy UNUSED
)
{
    struct seat *seat = udata;

    log_debug("Seat data device finished, removing seat...");
    wayland_del_seat(seat);
}

static const struct ext_data_control_device_v1_listener data_device_listener = {
    .data_offer = data_device_event_data_offer,
    .selection = data_device_event_selection,
    .primary_selection = data_device_event_primary_selection,
    .finished = data_device_event_finished
};

static void
seat_set(struct seat *seat, struct selection *ignore)
{
    if (seat->sel_regular.enabled && &seat->sel_regular != ignore)
        selection_set(&seat->sel_regular);
    if (seat->sel_primary.enabled && &seat->sel_primary != ignore)
        selection_set(&seat->sel_primary);
}

/*
 * Start listening to events from the seat, which should have a name. If seat is
 * already started, do nothing.
 */
static void
seat_start(struct seat *seat)
{
    if (seat->ext_data_device != NULL)
        return;

    log_debug("Starting seat \"%s\"", seat->name);

    seat->ext_data_device = ext_data_control_manager_v1_get_data_device(
        seat->wayland->ext_data_mgr, seat->proxy
    );
    ext_data_control_device_v1_add_listener(
        seat->ext_data_device, &data_device_listener, seat
    );

    seat->enabled = true;

    // Try setting each enabled selection in case an entry is already set.
    seat_set(seat, NULL);
}

static void
seat_stop(struct seat *seat)
{
    if (seat->ext_data_device == NULL)
        return;

    selection_uninit(&seat->sel_regular);
    selection_uninit(&seat->sel_primary);

    if (seat->ext_data_device != NULL)
    {
        ext_data_control_device_v1_destroy(seat->ext_data_device);
        seat->ext_data_device = NULL;
    }

    seat_clear_mime_types(seat);
}

static bool
wayland_seat_is_configured(
    struct wayland *wayland, const char *name, bool *regular, bool *primary
)
{
    struct config *config = wayland->config;

    if (xarray_len_config_seat(&config->configured_seats) == 0)
    {
        *regular = config->regular;
        *primary = config->primary;
        return true;
    }

    struct config_seat *config_seat;

    xarray_foreach(config_seat, &config->configured_seats, config_seat)
    {
        if (config_seat->name == NULL || strcmp(config_seat->name, name) == 0)
        {
            *regular = config_seat->regular;
            *primary = config_seat->primary;
            if (config_seat->regular || config_seat->primary)
                return true;
        }
    }
    return false;
}

/*
 * This is should always be called after we received all globals.
 */
static void
seat_event_name(void *udata, struct wl_seat *proxy UNUSED, const char *name)
{
    struct seat *seat = udata;

    log_debug("New seat name \"%s\"", name);

    if (seat->wayland->ext_data_mgr == NULL)
    {
        log_warn("Seat name event received before globals?");
        return;
    }

    free(seat->name);
    seat->name = strdup(name);
    if (seat->name == NULL)
    {
        log_errwarn("Error allocating seat name");
        return;
    }

    if (wayland_seat_is_configured(
            seat->wayland,
            name,
            &seat->sel_regular.enabled,
            &seat->sel_primary.enabled
        ))
        seat_start(seat);
    else
        seat_stop(seat);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = wayland_event_noop, .name = seat_event_name
};

static bool
wayland_add_seat(
    struct wayland *wayland, struct wl_seat *proxy, uint32_t global_name
)
{
    struct seat *seat = calloc(1, sizeof(*seat));

    if (seat == NULL)
    {
        log_errwarn("Error allocating seat structure");
        return false;
    }

    seat->wayland = wayland;
    seat->enabled = false;
    seat->proxy = proxy;
    seat->global_name = global_name;

    selection_init(&seat->sel_regular, seat);
    selection_init(&seat->sel_primary, seat);

    wl_seat_add_listener(proxy, &seat_listener, seat);

    xarray_init_mime_type(&seat->mime_types);

    xlist_insert_after_seat(&wayland->seats, seat);

    return true;
}

static void
wayland_del_seat(struct seat *seat)
{
    log_debug(
        "Removing seat \"%s\"", seat->name == NULL ? "(unknown)" : seat->name
    );

    selection_uninit(&seat->sel_regular);
    selection_uninit(&seat->sel_primary);

    wl_seat_destroy(seat->proxy);
    free(seat->name);

    if (seat->ext_data_device != NULL)
        ext_data_control_device_v1_destroy(seat->ext_data_device);

    seat_clear_mime_types(seat);

    xlist_unlink_seat(seat);
    free(seat);
}

static void
ftp_event_title(
    void                                         *udata,
    struct zwlr_foreign_toplevel_handle_v1 *proxy UNUSED,
    const char                                   *title
)
{
    struct toplevel *tp = udata;

    free(tp->title);
    tp->ack = false; // Set to false so that config is queried again
    log_debug("New title for %p: \"%s\"", tp, title);
    tp->title = strdup(title);
}

static void
ftp_event_app_id(
    void                                         *udata,
    struct zwlr_foreign_toplevel_handle_v1 *proxy UNUSED,
    const char                                   *app_id
)
{
    struct toplevel *tp = udata;

    free(tp->app_id);
    tp->ack = false;
    log_debug("New app_id for %p: \"%s\"", tp, app_id);
    tp->app_id = strdup(app_id);
}

static void
ftp_event_state(
    void                                         *udata,
    struct zwlr_foreign_toplevel_handle_v1 *proxy UNUSED,
    struct wl_array                              *states
)
{
    struct toplevel *tp = udata;

    enum zwlr_foreign_toplevel_handle_v1_state *state;

    wl_array_for_each(state, states)
    {
        if (*state == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
        {
            tp->activated = true;
            return;
        }
    }
    tp->activated = false;
}

static void
wayland_del_toplevel(struct toplevel *tp)
{
    if (tp->wayland->active_toplevel == tp)
        tp->wayland->active_toplevel = NULL;
    zwlr_foreign_toplevel_handle_v1_destroy(tp->proxy);
    free(tp->app_id);
    free(tp->title);

    xlist_unlink_toplevel(tp);
    free(tp);
}

static void
ftp_event_done(
    void *udata, struct zwlr_foreign_toplevel_handle_v1 *proxy UNUSED
)
{
    struct toplevel *tp = udata;
    struct wayland  *wayland = tp->wayland;

    // Check if toplevel is configured in config file now
    if (!tp->ack)
    {
        struct config_toplevel *config_tp;

        tp->config = NULL;
        xarray_foreach(
            config_toplevel, &wayland->config->configured_toplevels, config_tp
        )
        {
            if ((xarray_len_regex(&config_tp->titles) == 0 ||
                 (tp->title != NULL &&
                  match_regex_array(&config_tp->titles, tp->title))) &&
                (xarray_len_regex(&config_tp->app_ids) == 0 ||
                 (tp->app_id != NULL &&
                  match_regex_array(&config_tp->app_ids, tp->app_id))))
            {
                tp->config = config_tp;
                goto stop;
            }
        }

stop:
        tp->ack = true;
        if (tp->config == NULL)
        {
            if (wayland->active_toplevel == tp)
                goto no_active;
            return;
        }
    }
    else if (tp->config == NULL)
        return;

    if (tp->activated && wayland->active_toplevel != tp)
    {
        const char *title = tp->title == NULL ? "(unknown)" : tp->title;
        const char *app_id = tp->app_id == NULL ? "(unknown)" : tp->app_id;

        log_debug(
            "Toplevel %p (title: \"%s\", app_id: \"%s\") is active",
            tp,
            title,
            app_id
        );

        wayland->active_toplevel = tp;
    }
    else if (!tp->activated && wayland->active_toplevel == tp)
        goto no_active;

    return;
no_active:
    tp = wayland->active_toplevel;
    if (tp == NULL)
        return;

    const char *title = tp->title == NULL ? "(unknown)" : tp->title;
    const char *app_id = tp->app_id == NULL ? "(unknown)" : tp->app_id;

    log_debug(
        "Toplevel %p (title: \"%s\", app_id: \"%s\") is not active",
        tp,
        title,
        app_id
    );
    wayland->active_toplevel = NULL;
}

static void
ftp_event_closed(
    void *udata, struct zwlr_foreign_toplevel_handle_v1 *proxy UNUSED
)
{
    struct toplevel *tp = udata;

    const char *title = tp->title == NULL ? "(unknown)" : tp->title;
    const char *app_id = tp->app_id == NULL ? "(unknown)" : tp->app_id;

    log_debug(
        "Toplevel %p (title: \"%s\", app_id: \"%s\") closed", tp, title, app_id
    );

    wayland_del_toplevel(tp);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener ftp_listener = {
    .title = ftp_event_title,
    .app_id = ftp_event_app_id,
    .output_enter = wayland_event_noop,
    .output_leave = wayland_event_noop,
    .state = ftp_event_state,
    .done = ftp_event_done,
    .closed = ftp_event_closed,
    .parent = wayland_event_noop
};

static bool
wayland_add_toplevel(
    struct wayland *wayland, struct zwlr_foreign_toplevel_handle_v1 *proxy
)
{
    struct toplevel *tp = calloc(1, sizeof(*tp));

    if (tp == NULL)
    {
        log_errwarn("Error allocating toplevel structure");
        return false;
    }

    tp->wayland = wayland;
    tp->proxy = proxy;

    zwlr_foreign_toplevel_handle_v1_add_listener(proxy, &ftp_listener, tp);

    xlist_insert_after_toplevel(&wayland->toplevels, tp);

    return true;
}

static void
ftp_mgr_event_toplevel(
    void                                          *udata,
    struct zwlr_foreign_toplevel_manager_v1 *proxy UNUSED,
    struct zwlr_foreign_toplevel_handle_v1        *handle
)
{
    struct wayland *wayland = udata;

    if (!wayland_add_toplevel(wayland, handle))
        zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

static void
ftp_mgr_event_finished(
    void *udata, struct zwlr_foreign_toplevel_manager_v1 *proxy UNUSED
)
{
    struct wayland *wayland = udata;

    log_debug("Foreign toplevel manager finished");

    struct toplevel *tp;

    xlist_foreach_safe(toplevel, &wayland->toplevels, tp)
        wayland_del_toplevel(tp);

    zwlr_foreign_toplevel_manager_v1_destroy(wayland->ftp_mgr);
    wayland->ftp_mgr = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener ftp_mgr_listener =
    {.toplevel = ftp_mgr_event_toplevel, .finished = ftp_mgr_event_finished};

static void
registry_event_global(
    void               *udata,
    struct wl_registry *proxy,
    uint32_t            name,
    const char         *interface,
    uint32_t            version
)
{
    struct wayland *wayland = udata;

    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0)
    {
        wayland->ext_data_mgr = wl_registry_bind(
            proxy, name, &ext_data_control_manager_v1_interface, 1
        );
    }
    else if (
        strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0
    )
    {
        wayland->ftp_mgr = wl_registry_bind(
            proxy, name, &zwlr_foreign_toplevel_manager_v1_interface, 1
        );
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        if (version < WL_SEAT_NAME_SINCE_VERSION)
        {
            log_error(
                "wl_seat global version is below %d", WL_SEAT_NAME_SINCE_VERSION
            );
            return;
        }
        struct wl_seat *seat_proxy = wl_registry_bind(
            proxy, name, &wl_seat_interface, WL_SEAT_NAME_SINCE_VERSION
        );

        if (seat_proxy == NULL)
            log_errwarn("Error binding to seat proxy");
        else if (!wayland_add_seat(wayland, seat_proxy, name))
            wl_seat_destroy(seat_proxy);
    }
}

static void
registry_event_global_remove(
    void *udata, struct wl_registry *proxy UNUSED, uint32_t name
)
{
    struct wayland *wayland = udata;

    struct seat *seat;

    xlist_foreach_safe(seat, &wayland->seats, seat)
    {
        if (seat->global_name == name)
        {
            wayland_del_seat(seat);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_global,
    .global_remove = registry_event_global_remove
};

bool
wayland_init(
    struct wayland        *wayland,
    struct wayland_signals signals,
    struct eventloop      *loop,
    struct config         *config
)
{
    if (!wayland_ct_init(&wayland->wct, loop))
        return false;

    wayland->config = config;

    wl_registry_add_listener(
        wayland->wct.registry, &registry_listener, wayland
    );

    xlist_init_seat(&wayland->seats);
    xlist_init_toplevel(&wayland->toplevels);
    wayland->active_toplevel = NULL;
    wl_display_roundtrip(wayland->wct.display);

    if (wayland->ext_data_mgr == NULL)
    {
        log_error("ext-data-control-v1 protocol not supported by compositor");
        wayland_uninit(wayland);
        return false;
    }

    if (wayland->ftp_mgr != NULL)
    {
        log_debug("wlr-foreign-toplevel-management-v1 found");

        zwlr_foreign_toplevel_manager_v1_add_listener(
            wayland->ftp_mgr, &ftp_mgr_listener, wayland
        );
    }

    wayland->signals = signals;

    return true;
}

void
wayland_uninit(struct wayland *wayland)
{
    struct seat     *seat;
    struct toplevel *tp;

    xlist_foreach_safe(seat, &wayland->seats, seat) wayland_del_seat(seat);
    xlist_foreach_safe(toplevel, &wayland->toplevels, tp)
        wayland_del_toplevel(tp);

    if (wayland->ext_data_mgr != NULL)
        ext_data_control_manager_v1_destroy(wayland->ext_data_mgr);
    if (wayland->ftp_mgr)
        zwlr_foreign_toplevel_manager_v1_destroy(wayland->ftp_mgr);

    wayland_ct_uninit(&wayland->wct);
}

void
wayland_event_noop()
{
}

int
wayland_get_offer_fd(
    struct wayland                   *wayland,
    struct ext_data_control_offer_v1 *offer,
    const char                       *mime_type
)
{
    int fds[2];

    if (pipe(fds) == -1)
    {
        log_errerror("Error creating pipe");
        return -1;
    }

    ext_data_control_offer_v1_receive(offer, mime_type, fds[1]);
    // Close our write-end because we don't need it
    close(fds[1]);

    if (!wayland_ct_flush(&wayland->wct))
    {
        close(fds[0]);
        return -1;
    }

    return fds[0];
}

/*
 * Sync all enabled selections, except "ignore".
 */
void
wayland_set(struct wayland *wayland, struct selection *ignore)
{
    struct seat *seat;

    xlist_foreach(seat, &wayland->seats, seat)
    {
        if (seat->enabled)
            seat_set(seat, ignore);
    }
}
