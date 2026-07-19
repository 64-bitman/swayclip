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

    struct sc_list link;
};

static void
selection_init(struct selection *sel, struct seat *seat)
{
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

static const struct ext_data_control_device_v1_listener data_device_listener = {
    .data_offer = wayland_event_noop,
    .selection = wayland_event_noop,
    .primary_selection = wayland_event_noop,
    .finished = wayland_event_noop
};

/*
 * Start listening to events from the seat.
 */
static void
wayland_seat_start(struct seat *seat)
{
    seat->ext_data_device = ext_data_control_manager_v1_get_data_device(
        seat->wayland->ext_data_mgr, seat->proxy
    );
    ext_data_control_device_v1_add_listener(
        seat->ext_data_device, &data_device_listener, seat
    );

    seat->enabled = true;

    // If seat has an entry set for it, then become the source client. TODO
    if (false)
    {
    }
}

static bool
wayland_seat_is_configured(struct wayland *wayland, const char *name)
{
    struct config      *config = wayland->config;
    struct config_seat *config_seat;

    sc_array_foreach_ptr(&config->configured_seats, config_seat)
    {
        if (strcmp(config_seat->name, name) != 0)
            continue;
        if (config_seat->regular || config_seat->primary)
            return true;
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

    if (wayland_seat_is_configured(seat->wayland, name))
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

        // We may have binded to seats (and got seat name event) before the data
        // manager global. TODO
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
    struct wayland *wayland, struct eventloop *loop, struct config *config
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
