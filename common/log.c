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

#include "log.h"
#include "util.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define GREEN "\033[38;5;106m"
#define BLUE "\033[38;5;105m"
#define ORANGE "\033[38;5;208m"
#define RED "\033[38;5;196m"
#define GREY "\033[38;5;243m"
#define RESET "\033[0m"

static FILE          *LOG_FP = NULL;
static bool           LOG_COLOR = true;
static bool           FATAL = false;
static enum log_level LOG_LEVEL = LOG_INFO;

static const char *LEVEL_NAMES[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR"
};

static const char *LEVEL_COLORS[] = {
    [LOG_DEBUG] = GREEN,
    [LOG_INFO] = BLUE,
    [LOG_WARN] = ORANGE,
    [LOG_ERROR] = RED
};

/*
 * If "log_path" is NULL, then use stderr.
 */
void
log_init(const char *log_path, bool fatal)
{
    if (LOG_FP != NULL && LOG_FP != stderr)
        fclose(LOG_FP);
    if (log_path == NULL)
        LOG_FP = stderr;
    else
        LOG_FP = fopen(log_path, "w");

    if (LOG_FP == NULL)
        perror("Error opening log file for writing");

    const char *no_color = getenv("NO_COLOR");

    if ((no_color != NULL && *no_color != NUL) ||
        (LOG_FP == stderr && !isatty(STDERR_FILENO)) || log_path != NULL)
        LOG_COLOR = false;
    FATAL = fatal;
}

void
log_set_level(enum log_level level)
{
    LOG_LEVEL = level;
}

void
log_print_ex(
    enum log_level level, const char *file, int lnum, const char *fmt, ...
)
{
    if (FATAL && (level == LOG_WARN || level == LOG_ERROR))
    {
        fprintf(stderr, "Aborting!\n");
        exit(1);
    }
    if (LOG_FP == NULL || LOG_LEVEL > level)
        return;

    va_list ap;

    if (LOG_COLOR)
        fprintf(
            LOG_FP,
            "%s%s" RESET " " GREY "(%s:%d):" RESET " ",
            LEVEL_COLORS[level],
            LEVEL_NAMES[level],
            file,
            lnum
        );
    else
        fprintf(LOG_FP, "%s (%s:%d): ", LEVEL_NAMES[level], file, lnum);

    va_start(ap, fmt);
    vfprintf(LOG_FP, fmt, ap);
    va_end(ap);

    fputc('\n', LOG_FP);
    fflush(LOG_FP);
}
