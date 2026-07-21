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

#include "compositor.h"
#include "util.h"
#include <glib-unix.h>

/*
 * This will set $WAYLAND_DISPLAY environment variable. Should only have one
 * compositor instance at a time
 */
void
compositor_init(Compositor *comp)
{
    GSubprocess *proc;

    g_mkdir(g_get_user_runtime_dir(), 0755);

    g_autofree char *conf =
        g_build_filename(g_get_user_runtime_dir(), "swayconfig", NULL);

    ASSERT_NOERROR(g_file_set_contents(conf, "", -1, &ERROR));

    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_SEARCH_PATH_FROM_ENVP |
        G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    g_subprocess_launcher_setenv(
        launcher, "XDG_RUNTIME_DIR", g_get_user_runtime_dir(), TRUE
    );
    g_subprocess_launcher_setenv(launcher, "WLR_BACKENDS", "headless", TRUE);

    ASSERT_NOERROR(
        proc = g_subprocess_launcher_spawn(
            launcher, &ERROR, "sway", "-d", "-c", conf, NULL
        )
    );

    // Get WAYLAND_DISPLAY of sway instance
    GInputStream *in_stream = g_subprocess_get_stderr_pipe(proc);
    g_autoptr(GDataInputStream) stream = g_data_input_stream_new(in_stream);

    g_autoptr(GRegex) reg;

    ASSERT_NOERROR(
        reg = g_regex_new(
            "on wayland display '(.+)'",
            G_REGEX_DEFAULT,
            G_REGEX_MATCH_DEFAULT,
            &ERROR
        )
    );

    gboolean found = FALSE;

    while (TRUE)
    {
        g_autofree char *line;

        ASSERT_NOERROR(
            line =
                g_data_input_stream_read_line_utf8(stream, NULL, NULL, &ERROR)
        );

        if (line == NULL)
            break;

        g_autoptr(GMatchInfo) match;

        if (g_regex_match(reg, line, G_REGEX_MATCH_DEFAULT, &match))
        {
            g_autofree char *str = g_match_info_fetch(match, 1);

            g_assert_nonnull(str);
            comp->display =
                g_build_filename(g_get_user_runtime_dir(), str, NULL);

            found = TRUE;
            break;
        }
    }

    g_assert_true(found);

    comp->proc = proc;
}

void
compositor_uninit(Compositor *comp)
{
    g_subprocess_send_signal(comp->proc, SIGTERM);
    ASSERT_NOERROR(g_subprocess_wait(comp->proc, NULL, &ERROR));
    g_object_unref(comp->proc);

    g_free(comp->display);
}
