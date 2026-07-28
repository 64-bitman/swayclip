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

#include "client.h"
#include "compositor.h"
#include "daemon.h"
#include "ipc.h"
#include "util.h"
#include <glib.h>
#include <locale.h>
#include <sqlite3.h>
#include <sys/prctl.h>

typedef struct
{
    Compositor    comp;
    Daemon        daemon;
    struct ipc_ct ict;
    sqlite3      *db;
    Client       *client;
} Fixture;

static const char *CONFIG = "[daemon]\n"
                            "max_entries = 2\n"
                            "persist = true\n"
                            "regular = true\n"
                            "primary = true\n"
                            "[daemon.mime_types]\n"
                            "allowed = [\"text/.+\"]\n"
                            "blocked = [\"blocked\"]\n";

static void
fixture_setup(Fixture *fixture, const void *udata)
{
    compositor_init(&fixture->comp);
    daemon_init(&fixture->daemon, udata, fixture->comp.display);
    ASSERT_SQLITE(sqlite3_open_v2(
        fixture->daemon.db_file, &fixture->db, SQLITE_OPEN_READONLY, NULL
    ));
    fixture->client = client_new(fixture->comp.display);
}

static void
fixture_teardown(Fixture *fixture, const void *udata UNUSED)
{
    client_free(fixture->client);
    sqlite3_close(fixture->db);
    daemon_uninit(&fixture->daemon);
    compositor_uninit(&fixture->comp);
}

/*
 * Test if clipboard contants are saved into the database
 */
static void
test_clipboard_receive(Fixture *fixture, const void *udata UNUSED)
{
    client_copy(
        fixture->client, SELECTION_REGULAR, "one", "1", -1, "two", "2", -1, NULL
    );
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add(
        "/daemon/receive",
        Fixture,
        CONFIG,
        fixture_setup,
        test_clipboard_receive,
        fixture_teardown
    );

    // Terminate any child processes on exit
    g_assert_no_errno(prctl(PR_SET_PDEATHSIG, SIGTERM));

    return g_test_run();
}
