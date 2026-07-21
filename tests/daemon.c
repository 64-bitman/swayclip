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

#include "daemon.h"
#include "util.h"

void
daemon_init(Daemon *daemon, const char *config, const char *display)
{
    const char *path = g_getenv("TEST_BINARY");
    char       *dir;

    ASSERT_NOERROR(dir = g_dir_make_tmp("swayclip_test_XXXXXX", &ERROR));

    g_assert_nonnull(dir);

    g_autofree char *conf_file = g_build_filename(dir, "config.toml", NULL);

    ASSERT_NOERROR(g_file_set_contents(conf_file, config, -1, &ERROR));

    daemon->dir = dir;
    daemon->db_file = g_build_filename(dir, "history.sqlite3", NULL);

    g_autoptr(GSubprocessLauncher) launcher =
        g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);

    g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", display, TRUE);

    ASSERT_NOERROR(
        daemon->proc = g_subprocess_launcher_spawn(
            launcher, &ERROR, path, "-c", conf_file, "-s", dir, NULL
        )
    );

    g_assert_nonnull(daemon->proc);
}

void
daemon_uninit(Daemon *daemon)
{
    g_subprocess_send_signal(daemon->proc, SIGTERM);
    ASSERT_NOERROR(g_subprocess_wait(daemon->proc, NULL, &ERROR));
    g_object_unref(daemon->proc);

    char *argv[] = {"rm", "-rf", daemon->dir};
    ASSERT_NOERROR(g_spawn_sync(
        NULL,
        argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        &ERROR
    ));
    free(daemon->dir);
    free(daemon->db_file);
}
