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

#include "event.h"
#include <stdbool.h>
#include <wayland-client.h>

struct wayland_ct
{
    struct wl_display  *display;
    struct wl_registry *registry;

    int fd;
    int prepare_id;

    struct eventloop *loop;
    bool              read_prepared;
};

// clang-format off
bool wayland_ct_init(struct wayland_ct *wct, struct eventloop *loop);
void wayland_ct_uninit(struct wayland_ct *wct);
bool wayland_ct_flush(struct wayland_ct *wct);
// clang-format on
