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

#include "sc/sc_list.h"
#include <pthread.h>
#include <sys/epoll.h>

// Return true to remove source
typedef bool (*eventsource_callback)(int fd, int events, void *udata);

// Return true to remove source. Do not remove other prepare sources in the
// callback.
typedef bool (*eventprepare_callback)(void *udata);

enum event_priority
{
    EVENT_PRIORITY_HIGH,
    EVENT_PRIORITY_NORMAL
};

struct eventloop
{
    bool            stop;
    pthread_mutex_t stop_mut;

    int event_fd; // Used to wakeup loop
    int epoll;    // For normal priority sources

    struct sc_list sources;

    uint prepare_id;
    struct sc_list prepares;
};

// clang-format off
bool eventloop_init(struct eventloop *loop);
void eventloop_uninit(struct eventloop *loop);
bool eventloop_run(struct eventloop *loop);
void eventloop_wakeup(struct eventloop *loop);
void eventloop_stop(struct eventloop *loop);
bool eventloop_add(struct eventloop *loop, int fd, enum event_priority priority, int events, eventsource_callback callback, void *udata);
bool eventloop_del(struct eventloop *loop, int fd);
int eventloop_add_prepare(struct eventloop *loop, eventprepare_callback callback, void *udata);
bool eventloop_del_prepare(struct eventloop *loop, uint id);
// clang-format on
