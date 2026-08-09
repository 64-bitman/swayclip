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

#pragma once

#include "common/event.h"
#include "common/wayland_ct.h"
#include "config.h"
#include "protocols/ext-data-control-v1.h"
#include "protocols/wlr-foreign-toplevel-management-unstable-v1.h"
#include "xarray.h"
#include "xlist.h"
#include <wayland-client.h>

enum wayland_selection_type
{
    WAYLAND_SELECTION_REGULAR,
    WAYLAND_SELECTION_PRIMARY
};

struct wayland;
struct seat;
struct selection;
struct toplevel;

xarray_create(char *, mime_type, uint32_t, 10, 2);

struct wayland_signals
{
    // If "offer" is NULL, selection is cleared.
    struct
    {
        // clang-format off
        void (*callback)(struct selection *sel, struct ext_data_control_offer_v1 *offer, struct xarray_mime_type *mime_types, bool transient, void *udata);
        // clang-format on
        void *callback_udata;
    } selection;

    struct
    {
        // clang-format off
        void (*callback)(const char *mime_type, int fd, void *udata);
        // clang-format on
        void *callback_udata;
    } send;

    // Should return a source with all mime types added. Do not add a
    // listener to it. May return NULL to indicate nothing should be done,
    // unless "clear" is set to true, then clear thes election.
    struct
    {
        // clang-format off
        struct ext_data_control_source_v1 *(*callback)(struct ext_data_control_manager_v1 *mgr, bool *clear, void *udata);
        // clang-format on
        void *callback_udata;
    } set;

    // Should return true if there is something to set the selection to
    struct
    {
        bool (*callback)(void *udata);
        void *callback_udata;
    } can_set;
};

xlist_declare(seat);
xlist_declare(toplevel);

struct wayland
{
    struct wayland_ct wct;
    struct config    *config;

    struct ext_data_control_manager_v1      *ext_data_mgr;
    struct zwlr_foreign_toplevel_manager_v1 *ftp_mgr; // May be NULL

    struct xlist_seat     seats;
    struct xlist_toplevel toplevels;

    // Toplevel that is currently active, or NULL if none
    struct toplevel *active_toplevel;

    struct wayland_signals signals;
};

// clang-format off
bool wayland_init(struct wayland *wayland, struct wayland_signals signals, struct eventloop *loop, struct config *config);
void wayland_uninit(struct wayland *wayland);
void wayland_event_noop();
int wayland_get_offer_fd(struct wayland *wayland, struct ext_data_control_offer_v1 *offer, const char *mime_type);
void wayland_set(struct wayland *wayland, struct selection *ignore);
// clang-format on
