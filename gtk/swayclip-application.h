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

#include <gtk/gtk.h>

#define SWAYCLIP_TYPE_APPLICATION (swayclip_application_get_type())
// clang-format off
G_DECLARE_FINAL_TYPE(SwayclipApplication, swayclip_application, SWAYCLIP, APPLICATION, GtkApplication)
// clang-format on

// clang-format off
SwayclipApplication *swayclip_application_new(void);
// clang-format on
