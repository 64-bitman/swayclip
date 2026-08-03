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

#include "swayclip-application.h"
#include "swayclip-connection.h"

struct _SwayclipApplication
{
    GtkApplication parent;

    SwayclipConnection *ct;
};

G_DEFINE_TYPE(SwayclipApplication, swayclip_application, GTK_TYPE_APPLICATION)

static void
swayclip_application_class_init(SwayclipApplicationClass *class)
{
}

static void
swayclip_application_init(SwayclipApplication *self)
{
}

SwayclipApplication *
swayclip_application_new(void)
{
    SwayclipApplication *app = g_object_new(
        SWAYCLIP_TYPE_APPLICATION,
        "application-id",
        "com.github.swayclip",
        "flags",
        G_APPLICATION_CAN_OVERRIDE_APP_ID,
        NULL
    );

    return app;
}
