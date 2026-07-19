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
#include "common/sc/sc_array.h"
#include "common/sc/sc_list.h"
#include "common/wayland_ct.h"
#include "config.h"
#include "protocol/ext-data-control-v1.h"
#include <wayland-client.h>

enum wayland_selection_type
{
    WAYLAND_SELECTION_REGULAR,
    WAYLAND_SELECTION_PRIMARY
};

struct wayland
{
    struct wayland_ct wct;
    struct config    *config;

    struct ext_data_control_manager_v1 *ext_data_mgr;
    struct sc_list                      seats;

    struct
    {
        // clang-format off
        // If "offer" is NULL, selection is cleared.
        void (*callback)(struct wayland *wayland, struct ext_data_control_offer_v1 *offer, const struct sc_array_astr *mime_types);
        // clang-format on
        void *callback_udata;
    } signal_selection;

    struct
    {
        // clang-format off
        void (*callback)(struct wayland *wayland, const char *mime_type, int fd);
        // clang-format on
        void *callback_udata;
    } signal_send;
};

// clang-format off
bool wayland_init(struct wayland *wayland, struct eventloop *loop, struct config *config);
void wayland_uninit(struct wayland *wayland);
void wayland_event_noop();
// clang-format on
