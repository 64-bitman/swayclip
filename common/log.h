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

#include "util.h"
#include <errno.h>
#include <stdarg.h>
#include <string.h> // IWYU pragma: keep

enum log_level
{
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_NONE
};

#define log_print(l, fmt, ...)                                                 \
    log_print_ex(l, __FILE_NAME__, __LINE__, fmt, ##__VA_ARGS__)
#define log_err(l, fmt, ...)                                                   \
    log_print(l, fmt ": %s", ##__VA_ARGS__, strerror(errno))

#define log_debug(fmt, ...) log_print(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) log_print(LOG_INFO, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_print(LOG_WARN, fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_print(LOG_ERROR, fmt, ##__VA_ARGS__)

#define log_errdebug(fmt, ...) log_err(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_errinfo(fmt, ...) log_err(LOG_INFO, fmt, ##__VA_ARGS__)
#define log_errwarn(fmt, ...) log_err(LOG_WARN, fmt, ##__VA_ARGS__)
#define log_errerror(fmt, ...) log_err(LOG_ERROR, fmt, ##__VA_ARGS__)

#define log_abort(fmt, ...)                                                    \
    do                                                                         \
    {                                                                          \
        log_error(fmt, ##__VA_ARGS__);                                         \
        abort();                                                               \
    } while (false)
#define log_errabort(fmt, ...)                                                 \
    log_abort(fmt ": %s", ##__VA_ARGS__, strerror(errno));

// clang-format off
void log_init(const char *log_path);
void log_set_level(enum log_level level);
void log_print_ex( enum log_level level, const char *file, int lnum, const char *fmt, ...) PRINTFLIKE(4, 5);
// clang-format on
