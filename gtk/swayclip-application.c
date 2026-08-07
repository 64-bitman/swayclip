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
swayclip_application_dispose(GObject *obj)
{
    SwayclipApplication *self = SWAYCLIP_APPLICATION(obj);

    g_clear_object(&self->ct);

    G_OBJECT_CLASS(swayclip_application_parent_class)->dispose(obj);
}

static void
swayclip_application_activate(GApplication *app)
{
    SwayclipApplication *self = SWAYCLIP_APPLICATION(app);

    GtkWindow *win =
        GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(app)));

    gtk_window_set_title(win, "swayclip");
    gtk_window_set_default_size(win, 400, 300);

    // Root GtkBox that contains everything
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_window_set_child(win, vbox);

    gtk_window_present(win);
}

static void
swayclip_application_startup(GApplication *app)
{
    SwayclipApplication *self = SWAYCLIP_APPLICATION(app);

    self->ct = swayclip_connection_new();

    G_APPLICATION_CLASS(swayclip_application_parent_class)->startup(app);
}

static void
swayclip_application_class_init(SwayclipApplicationClass *class)
{
    GObjectClass      *obj_class = G_OBJECT_CLASS(class);
    GApplicationClass *app_class = G_APPLICATION_CLASS(class);

    obj_class->dispose = swayclip_application_dispose;

    app_class->startup = swayclip_application_startup;
    app_class->activate = swayclip_application_activate;
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
