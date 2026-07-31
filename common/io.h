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

#include "xarray.h"
#include <time.h>

// If changing "len" type, update io_read()!
xarray_create(uint8_t, io, uint32_t, 4096, 2);
struct io_read
{
    // Set by caller
    int      fd;
    uint8_t *buf;
    size_t   bufsize;
    void (*data_callback)(uint8_t *buf, ssize_t len, void *udata);
    void *callback_udata;

    // Set by function
    struct xarray_io data;
};

struct io_write
{
    // Set by caller
    int      fd;
    uint8_t *buf;
    size_t   bufsize;
    // clang-format off
    bool (*data_callback)(uint8_t *buf, size_t sz, size_t *len, void *udata);
    // clang-format on
    void *callback_udata;
};

// clang-format off
int64_t get_time_ns(clockid_t id);
bool set_fd_nonblocking(int fd);
bool io_read(struct io_read *ctx, int timeout, size_t max_bytes);
bool io_write(struct io_write *ctx, int timeout);
ssize_t io_recv(int fd, uint8_t *buf, size_t len, int *scm_fd, bool *poll);
ssize_t io_send(int buf, uint8_t *bf, size_t len, int scm_fd, bool *poll);
int create_lock(const char *path, int *lock_fd);
pid_t lock_is_locked(const char *path);
// clang-format on
