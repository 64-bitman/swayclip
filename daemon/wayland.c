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

    struct sc_array_astr mime_types;
    bool                 blocked;

    struct sc_list link;
};

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
        ext_data_control_offer_v1_destroy(sel->ext_data_offer);
    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);

    if (sel->null_timerfd != -1)
    {
        eventloop_del(sel->seat->wayland->wct.loop, sel->null_timerfd);
        close(sel->null_timerfd);
    }
}

static void
selection_set(struct selection *sel)
{
    struct wayland *wayland = sel->seat->wayland;
    bool            clear = false;

    log_debug(
        "Setting selection %d for seat \"%s\"", sel->type, sel->seat->name
    );

    struct ext_data_control_source_v1 *source = wayland->signals.set.callback(
        sel->seat->ext_data_device, &clear, wayland->signals.set.callback_udata
    );

    if (!clear && source == NULL)
        return;

    if (sel->ext_data_source != NULL)
        ext_data_control_source_v1_destroy(sel->ext_data_source);
    sel->ext_data_source = source;

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
match_regex_array(struct sc_array_regex *arr, const char *target)
{
    regex_t *reg;
    sc_array_foreach_ptr(arr, reg)
    {
        if (regexec(reg, target, 0, NULL, 0) == 0)
            return true;
    }
    return false;
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

    // Do not save entry if mime type is configured to be blocked.
    if (sc_array_size(&config->blocked_mime_types) > 0 &&
        match_regex_array(&config->blocked_mime_types, mime_type))
        seat->blocked = true;

    if (seat->blocked)
        return;

    // Check if mime type is allowed to be saved
    if (sc_array_size(&config->allowed_mime_types) > 0 &&
        !match_regex_array(&config->allowed_mime_types, mime_type))
        return;

    char *str = strdup(mime_type);

    if (str == NULL)
        return;
    sc_array_add(&seat->mime_types, str);
}

static const struct ext_data_control_offer_v1_listener data_offer_listener = {
    .offer = data_offer_event_offer
};

static void
seat_clear_mime_types(struct seat *seat)
{
    char *mime_type;
    sc_array_foreach(&seat->mime_types, mime_type) free(mime_type);
    sc_array_term(&seat->mime_types);
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

    ext_data_control_offer_v1_add_listener(
        offer_proxy, &data_offer_listener, seat
    );
}

static bool
null_timer_callback(int fd, int events, void *udata)
{
    if (events & (EPOLLHUP | EPOLLERR))
    {
        log_errwarn("Error polling timer fd for null check");
        return true;
    }
    if (!(events & EPOLLIN))
        return false;

    uint64_t exp;
    ssize_t  r = read(fd, &exp, sizeof(exp));

    if (r == -1)
    {
        log_errwarn("Error reading timer fd for null check");
        return true;
    }

    struct selection *sel = udata;

    if (sel->ext_data_offer == NULL)
        // NULL selection event is valid, become the source client
        selection_set(sel);

    return true;
}

static void
selection_event(
    struct seat                      *seat,
    struct ext_data_control_offer_v1 *offer,
    struct selection                 *sel
)
{
    if (!sel->enabled)
    {
        if (offer != NULL)
            ext_data_control_offer_v1_destroy(offer);
        return;
    }

    if (sel->ext_data_offer != NULL)
        ext_data_control_offer_v1_destroy(sel->ext_data_offer);

    if (sel->ext_data_source != NULL || seat->blocked)
    {
        // Currently source client or blocked, ignore
        if (offer != NULL)
            ext_data_control_offer_v1_destroy(offer);
        sel->ext_data_offer = NULL;
        return;
    }

    sel->ext_data_offer = offer;

    if (offer == NULL)
    {
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
                seat->wayland->wct.loop,
                fd,
                EVENT_PRIORITY_NORMAL,
                EPOLLIN,
                null_timer_callback,
                sel
            ))
            close(fd);
        return;
    }

    struct wayland_signals *signals = &seat->wayland->signals;

    signals->selection.callback(
        offer, &seat->mime_types, signals->selection.callback_udata
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
seat_set(struct seat *seat)
{
    if (seat->sel_regular.enabled)
        selection_set(&seat->sel_regular);
    if (seat->sel_primary.enabled)
        selection_set(&seat->sel_primary);
}

/*
 * Start listening to events from the seat, which should have a name.
 */
static void
wayland_seat_start(struct seat *seat)
{
    log_debug("Starting seat \"%s\"", seat->name);

    seat->ext_data_device = ext_data_control_manager_v1_get_data_device(
        seat->wayland->ext_data_mgr, seat->proxy
    );
    ext_data_control_device_v1_add_listener(
        seat->ext_data_device, &data_device_listener, seat
    );

    seat->enabled = true;

    // Try setting each enabled selection in case an entry is already set.
    seat_set(seat);
}

static bool
wayland_seat_is_configured(
    struct wayland *wayland, const char *name, bool *regular, bool *primary
)
{
    struct config      *config = wayland->config;
    struct config_seat *config_seat;

    if (sc_array_size(&config->configured_seats) == 0)
    {
        *regular = config->regular;
        *primary = config->primary;
        return true;
    }

    sc_array_foreach_ptr(&config->configured_seats, config_seat)
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
        wayland_seat_start(seat);
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

    sc_array_init(&seat->mime_types);

    sc_list_init(&seat->link);
    sc_list_add_head(&wayland->seats, &seat->link);

    return true;
}

static void
wayland_del_seat(struct seat *seat)
{
    selection_uninit(&seat->sel_regular);
    selection_uninit(&seat->sel_primary);

    wl_seat_destroy(seat->proxy);
    free(seat->name);

    if (seat->ext_data_device != NULL)
        ext_data_control_device_v1_destroy(seat->ext_data_device);

    seat_clear_mime_types(seat);

    sc_list_del(&seat->link);
    free(seat);
}

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
    struct sc_list *it, *tmp;

    sc_list_foreach_safe(&wayland->seats, tmp, it)
    {
        struct seat *seat = sc_list_entry(it, struct seat, link);

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

    sc_list_init(&wayland->seats);
    wl_display_roundtrip(wayland->wct.display);

    if (wayland->ext_data_mgr == NULL)
    {
        log_error("ext-data-control-v1 protocol not supported by compositor");
        wayland_uninit(wayland);
        return false;
    }

    wayland->signals = signals;

    return true;
}

void
wayland_uninit(struct wayland *wayland)
{
    struct sc_list *it, *tmp;

    sc_list_foreach_safe(&wayland->seats, tmp, it)
    {
        struct seat *seat = sc_list_entry(it, struct seat, link);

        wayland_del_seat(seat);
    }

    if (wayland->ext_data_mgr != NULL)
        ext_data_control_manager_v1_destroy(wayland->ext_data_mgr);

    wayland_ct_uninit(&wayland->wct);
}

void
wayland_event_noop()
{
}
