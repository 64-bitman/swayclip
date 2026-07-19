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

#include "wayland_ct.h"
#include "log.h"

static bool
display_prepare_callback(void *udata)
{
    struct wayland_ct *wct = udata;

    while (wl_display_prepare_read(wct->display) == -1)
        if (wl_display_dispatch_pending(wct->display) == -1)
        {
            log_errerror("Error dispatching Wayland events");
            goto exit;
        }

    if (wl_display_flush(wct->display) == -1)
    {
        log_errerror("Error flushing Wayland display");
        goto exit;
    }

    return false;
exit:
    eventloop_stop(wct->loop);
    return true;
}

static bool
display_callback(int fd UNUSED, int events, void *udata)
{
    struct wayland_ct *wct = udata;

    if (events & EPOLLIN)
    {
        if (wl_display_read_events(wct->display) == -1)
        {
            log_errerror("Error reading events from Wayland display");
            goto exit;
        }
        if (wl_display_dispatch_pending(wct->display) == -1)
        {
            log_errerror("Error dispatching Wayland events");
            goto exit;
        }
    }
    else if (events & (EPOLLHUP | EPOLLERR))
        goto exit;

    return false;
exit:
    eventloop_stop(wct->loop);
    return true;
}

bool
wayland_ct_init(struct wayland_ct *wct, struct eventloop *loop)
{
    wct->display = wl_display_connect(NULL);

    if (wct->display == NULL)
    {
        log_errerror("Error connecting to Wayland display");
        return false;
    }

    wct->fd = wl_display_get_fd(wct->display);
    wct->loop = loop;

    wct->prepare_id =
        eventloop_add_prepare(loop, display_prepare_callback, wct);
    if (wct->prepare_id == -1)
    {
        wl_display_disconnect(wct->display);
        return false;
    }

    // Make it high priority, so that Wayland events are always processed before
    // anything else.
    if (!eventloop_add(
            loop, wct->fd, EVENT_PRIORITY_HIGH, EPOLLIN, display_callback, wct
        ))
    {
        wl_display_disconnect(wct->display);
        eventloop_del_prepare(loop, wct->prepare_id);
        return false;
    }

    wct->registry = wl_display_get_registry(wct->display);

    return true;
}

void
wayland_ct_uninit(struct wayland_ct *wct)
{
    eventloop_del(wct->loop, wct->fd);
    eventloop_del_prepare(wct->loop, wct->prepare_id);
    wl_registry_destroy(wct->registry);
    wl_display_disconnect(wct->display);
}
