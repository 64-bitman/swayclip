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

#include "event.h"
#include "log.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct eventsource
{
    int                 fd;
    int                 events;
    enum event_priority priority;

    eventsource_callback callback;
    void                *callback_udata;

    struct xlist_eventsource link;
};
xlist_define(eventsource, struct eventsource, link);

struct eventprepare
{
    uint id;

    eventprepare_callback callback;
    void                 *callback_udata;

    struct xlist_eventprepare link;
};
xlist_define(eventprepare, struct eventprepare, link);

bool
eventloop_init(struct eventloop *loop)
{
    loop->epoll = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll == -1)
    {
        log_errerror("Error creating epoll fd");
        close(loop->epoll);
        return false;
    }

    loop->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    struct epoll_event ev = {.events = EPOLLIN, .data.ptr = NULL};
    if (loop->event_fd == -1 ||
        epoll_ctl(loop->epoll, EPOLL_CTL_ADD, loop->event_fd, &ev) == -1)
    {
        if (loop->event_fd != -1)
        {
            log_errerror("Error adding event fd to epoll");
            close(loop->event_fd);
        }
        else
            log_errerror("Error creating event fd");

        close(loop->epoll);
        return false;
    }

    loop->stop = true;
    loop->prepare_id = 0;
    pthread_mutex_init(&loop->stop_mut, NULL);

    xlist_init_eventsource(&loop->sources);
    xlist_init_eventsource(&loop->deleted_sources);
    xlist_init_eventprepare(&loop->prepares);
    return true;
}

static void
eventsource_del(struct eventsource *source, struct eventloop *loop)
{
    if (epoll_ctl(loop->epoll, EPOLL_CTL_DEL, source->fd, NULL) == -1)
        if (errno != EBADF)
            log_errwarn("Error deleting fd %d from epoll", source->fd);

    xlist_unlink_eventsource(source);
    free(source);
}

static void
eventprepare_del(struct eventprepare *prepare)
{
    xlist_unlink_eventprepare(prepare);
    free(prepare);
}

void
eventloop_uninit(struct eventloop *loop)
{
    if (!xlist_empty_eventsource(&loop->sources))
    {
        log_warn("Event loop still has sources active");

        struct eventsource *source;
        xlist_foreach_safe(eventsource, &loop->sources, source)
            eventsource_del(source, loop);
    }
    if (!xlist_empty_eventprepare(&loop->prepares))
    {
        log_warn("Event loop still has prepare sources active");

        struct eventprepare *prepare;
        xlist_foreach_safe(eventprepare, &loop->prepares, prepare)
            eventprepare_del(prepare);
    }

    close(loop->epoll);
    close(loop->event_fd);

    pthread_mutex_destroy(&loop->stop_mut);
}

/*
 * Run the event loop until it is stopped. Returns true on success and false on
 * failure.
 */
bool
eventloop_run(struct eventloop *loop)
{
#define MAX_EVENTS 4
    struct epoll_event  evs[MAX_EVENTS];
    struct epoll_event  high[MAX_EVENTS];
    struct epoll_event  norm[MAX_EVENTS];
    struct epoll_event *buckets[2] = {high, norm};

    loop->stop = false;

    while (true)
    {
        int n_high = 0;
        int n_norm = 0;

        struct eventprepare *prepare;

        xlist_foreach_safe(eventprepare, &loop->prepares, prepare)
        {
            if (prepare->callback(prepare->callback_udata))
                eventprepare_del(prepare);
        }

        int n_ev;
        while (true)
        {
            n_ev = epoll_wait(loop->epoll, evs, MAX_EVENTS, -1);

            if (n_ev == -1)
            {
                if (errno == EINTR)
                    continue;
                log_errerror("epoll_wait() error");
                return false;
            }
            break;
        }

        for (int i = 0; i < n_ev; i++)
        {
            struct eventsource *source = evs[i].data.ptr;

            if (evs[i].data.ptr == NULL)
            {
                uint64_t val;
                ssize_t  r = read(loop->event_fd, &val, sizeof(val));

                if (r == -1)
                    log_errwarn("Error reading event fd");
                continue;
            }

            if (source->priority == EVENT_PRIORITY_HIGH)
                high[n_high++] = evs[i];
            else
                norm[n_norm++] = evs[i];
        }

        for (int i = 0; i < 2; i++)
        {
            struct epoll_event *bucket = buckets[i];
            int                 n = bucket == high ? n_high : n_norm;

            for (int k = 0; k < n; k++)
            {
                struct eventsource *source = bucket[k].data.ptr;

                if (source->callback(
                        source->fd, bucket[k].events, source->callback_udata
                    ))
                {
                    xlist_unlink_eventsource(source);
                    xlist_insert_after_eventsource(
                        &loop->deleted_sources, source
                    );
                }
            }
        }

        struct eventsource *source;

        xlist_foreach_safe(eventsource, &loop->deleted_sources, source)
        {
            eventsource_del(source, loop);
        }

        pthread_mutex_lock(&loop->stop_mut);
        bool stop = loop->stop;
        pthread_mutex_unlock(&loop->stop_mut);

        if (stop)
            break;
    }

#undef MAX_EVENTS
    return true;
}

void
eventloop_wakeup(struct eventloop *loop)
{
    uint64_t i = 1;
    write(loop->event_fd, &i, sizeof(i));
}

void
eventloop_stop(struct eventloop *loop)
{
    pthread_mutex_lock(&loop->stop_mut);
    loop->stop = true;
    pthread_mutex_unlock(&loop->stop_mut);
    eventloop_wakeup(loop);
}

/*
 * Add fd to event loop with the given priority. Returns true on success and
 * false on failure.
 */
bool
eventloop_add(
    struct eventloop    *loop,
    int                  fd,
    enum event_priority  priority,
    int                  events,
    eventsource_callback callback,
    void                *udata
)
{
    struct eventsource *source = malloc(sizeof(*source));

    if (source == NULL)
    {
        log_errerror("Error allocating event source");
        return false;
    }

    struct epoll_event ev = {
        .events = events,
        .data.ptr = source,
    };

    if (epoll_ctl(loop->epoll, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        log_errerror("Error adding fd to epoll fd");
        free(source);
        return false;
    }

    source->fd = fd;
    source->events = events;
    source->priority = priority;
    source->callback = callback;
    source->callback_udata = udata;

    xlist_insert_after_eventsource(&loop->sources, source);

    return true;
}

static struct eventsource *
eventloop_find(struct xlist_eventsource *list, int fd)
{
    struct eventsource *source;

    xlist_foreach(eventsource, list, source)
    {
        if (source->fd == fd)
            return source;
    }
    return NULL;
}

/*
 * Remove fd from event loop, note that this does not close the fd. Return true
 * if fd was found
 */
bool
eventloop_del(struct eventloop *loop, int fd)
{
    struct eventsource *source = eventloop_find(&loop->sources, fd);

    if (loop->stop)
    {
        // Delete source now
        if (source == NULL)
            source = eventloop_find(&loop->deleted_sources, fd);
        if (source != NULL)
        {
            eventsource_del(source, loop);
            return true;
        }
        return false;
    }

    if (source == NULL)
        return false;
    xlist_unlink_eventsource(source);
    xlist_insert_after_eventsource(&loop->deleted_sources, source);
    return true;
}

/*
 * Modify events of fd. Return true on success and false on failure
 */
bool
eventloop_mod(struct eventloop *loop, int fd, int events)
{
    struct eventsource *source = eventloop_find(&loop->sources, fd);

    if (source == NULL)
        return false;
    if (source->events == events)
        return true;

    struct epoll_event ev = {.events = events, .data.ptr = source};

    if (epoll_ctl(loop->epoll, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        log_errerror("Error modifying epoll fd");
        return false;
    }

    source->events = events;
    return true;
}

/*
 * Add prepare source to event loop. Return ID that can be used to remove it.
 * Returns -1 on failure.
 */
int
eventloop_add_prepare(
    struct eventloop *loop, eventprepare_callback callback, void *udata
)
{
    struct eventprepare *prepare = malloc(sizeof(*prepare));

    if (prepare == NULL)
    {
        log_errerror("Error allocating event prepare source");
        return -1;
    }

    prepare->id = loop->prepare_id++;
    prepare->callback = callback;
    prepare->callback_udata = udata;

    xlist_insert_after_eventprepare(&loop->prepares, prepare);

    return prepare->id;
}

/*
 * Remove prepare source from event loop, return true if removed.
 */
bool
eventloop_del_prepare(struct eventloop *loop, uint id)
{
    struct eventprepare *prepare;

    xlist_foreach_safe(eventprepare, &loop->prepares, prepare)
    {
        if (prepare->id == id)
        {
            xlist_unlink_eventprepare(prepare);
            free(prepare);
            return true;
        }
    }
    return false;
}
