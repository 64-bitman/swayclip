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

#include "common/event.h"
#include "common/log.h"
#include "common/version.h"
#include "config.h"
#include "database.h"
#include "wayland.h"
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signalfd.h>

struct state
{
    struct eventloop loop;

    struct config config;

    struct wayland  wayland;
    struct database db;
};

static bool
signal_callback(int fd, int events UNUSED, void *udata)
{
    struct signalfd_siginfo sfd_info;

    ssize_t r = read(fd, &sfd_info, sizeof(sfd_info));

    if (r == -1)
    {
        log_errwarn("Error reading signal fd");
        return false;
    }

    if (sfd_info.ssi_signo == SIGINT || sfd_info.ssi_signo == SIGTERM)
    {
        struct eventloop *loop = udata;

        eventloop_stop(loop);
        log_info("Exiting...");
        return true;
    }
    return false;
}

static void
wsignal_selection(
    struct ext_data_control_offer_v1 *offer,
    const struct sc_array_astr       *mime_types,
    void                             *udata
)
{
    struct state *state = udata;

    (void)offer;
    (void)mime_types;
    (void)state;
}

static void
wsignal_send(const char *mime_type, int fd, void *udata)
{
    struct state *state = udata;

    (void)mime_type;
    (void)fd;
    (void)state;
}

static struct ext_data_control_source_v1 *
wsignal_set(struct ext_data_control_device_v1 *device, bool *clear, void *udata)
{
    struct state *state = udata;

    (void)device;
    (void)clear;
    (void)state;

    return NULL;
}

int
main(int argc, char **argv)
{
    static const struct option options[] = {
        {"logfile", required_argument, 0, 'l'},
        {"config", required_argument, 0, 'c'},
        {"db", required_argument, 0, 's'},
        {"debug", no_argument, 0, 'd'},
        {"version", no_argument, 0, 'v'},
        {NULL, 0, 0, 0}
    };

    int   c;
    int   idx;
    bool  init_log = false;
    char *config = NULL;
    char *db = NULL;

    while ((c = getopt_long(argc, argv, "l:c:s:dv", options, &idx)) != -1)
    {
        switch (c)
        {
        case 'l':
            log_init(optarg);
            init_log = true;
            break;
        case 'c':
            config = strdup(optarg);
            break;
        case 's':
            db = strdup(optarg);
            break;
        case 'd':
            log_set_level(LOG_DEBUG);
            break;
        case 'v':
            printf("%s\n", PROJECT_VERSION);
            break;
        default:
            free(config);
            free(db);
            return EXIT_FAILURE;
        }
    }

    if (!init_log)
        log_init(NULL);

    sigset_t block;

    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGPIPE);

    if (pthread_sigmask(SIG_BLOCK, &block, NULL) == -1)
    {
        log_errerror("Error setting signal mask");
        free(config);
        free(db);
        return EXIT_FAILURE;
    }

    struct state state;
    bool         ret = false;

    ret = config_init(&state.config, config);
    free(config);
    if (!ret)
        return EXIT_FAILURE;

    if (!eventloop_init(&state.loop))
    {
        config_uninit(&state.config);
        free(db);
        return EXIT_FAILURE;
    }

    struct wayland_signals wsignals = {
        .selection = {.callback = wsignal_selection, .callback_udata = &state},
        .send = {.callback = wsignal_send, .callback_udata = &state},
        .set = {.callback = wsignal_set, .callback_udata = &state}
    };

    ret = database_init(&state.db, db, &state.config);
    free(db);
    if (!ret)
    {
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

    if (!wayland_init(&state.wayland, wsignals, &state.loop, &state.config))
    {
        database_uninit(&state.db);
        config_uninit(&state.config);
        eventloop_uninit(&state.loop);
        return EXIT_FAILURE;
    }

    int sig_fd = signalfd(-1, &block, SFD_NONBLOCK | SFD_CLOEXEC);

    if (sig_fd == -1 || !eventloop_add(
                            &state.loop,
                            sig_fd,
                            EVENT_PRIORITY_NORMAL,
                            EPOLLIN,
                            signal_callback,
                            &state.loop
                        ))
    {
        log_errerror("Error setting up signal mask");
        goto exit;
    }

    ret = eventloop_run(&state.loop);

exit:
    if (sig_fd != -1)
    {
        (void)eventloop_del(&state.loop, sig_fd);
        close(sig_fd);
    }

    wayland_uninit(&state.wayland);
    database_uninit(&state.db);
    config_uninit(&state.config);
    eventloop_uninit(&state.loop);

    return ret ? EXIT_SUCCESS : EXIT_FAILURE;
}
