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

#include "ipc.h"
#include <sys/socket.h>
#include <sys/un.h>

void
init_ipc(struct ipc_ct *ict, const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, NULL);

    g_assert_no_errno(fd);

    struct sockaddr_un addr;

    g_snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    addr.sun_family = AF_UNIX;

    g_assert_no_errno(connect(fd, (struct sockaddr *)&addr, sizeof(addr)));

    g_assert_true(ipc_ct_init(ict, fd));
}
