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

#include "sc/sc_array.h"
#include <stddef.h>
#include <time.h>

struct io_read
{
    // Set by called
    int      fd;
    uint8_t *buf;
    size_t   bufsize;
    void (*data_callback)(uint8_t *, ssize_t, void *);
    void *callback_udata;

    // Set by function
    struct sc_array_8 arr;
};

// clang-format off
int64_t get_time_ns(clockid_t id);
bool set_fd_nonblocking(int fd);
bool io_read(struct io_read *ctx, int timeout);
// clang-format on
