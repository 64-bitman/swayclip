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

#include "config_util.h"
#include "log.h"
#include "util.h"
#include "xdg.h"
#include <stdlib.h>

/*
 * Parse the given config file and return result in "result". If "file" is
 * NULL, then use "$XDG_CONFIG_HOME/wlip/config.toml". Returns true on success
 * and false on failure.
 */
bool
config_parse(const char *file, toml_result_t *result)
{
    char *tofree = NULL;

    if (file == NULL)
    {
        char *dir = xdg_get_base_dir(XDG_CONFIG_HOME, "wlip");

        if (dir == NULL)
            return false;
        tofree = xstrdup_printf("%s/config.toml", dir);
        file = tofree;
        free(dir);
    }

    if (file == NULL)
    {
        log_errerror("Error allocating config file path");
        return false;
    }

    toml_result_t res = toml_parse_file_ex(file);

    free(tofree);

    if (!res.ok)
    {
        log_error("Error parsing config file: %s", res.errmsg);
        return false;
    }

    *result = res;

    return true;
}

static const char *
type_to_str(toml_type_t type)
{
    switch (type)
    {
    case TOML_TABLE:
        return "table";
    case TOML_ARRAY:
        return "array";
    case TOML_STRING:
        return "string";
    case TOML_INT64:
        return "integer";
    case TOML_BOOLEAN:
        return "boolean";
    default:
        return "unknown";
    }
}

/*
 * Extract the options from "table". Returns true on success and false on
 * failure.
 */
bool
config_extract(toml_datum_t table, const struct config_option *opts, int n_opts)
{
    for (int i = 0; i < n_opts; i++)
    {
        const struct config_option *opt = opts + i;

        toml_datum_t dat = toml_seek(table, opt->key);
        toml_type_t  type = dat.type;

        if (type == TOML_UNKNOWN)
            continue;

        if (type != opt->type)
        {
            log_error(
                "Config option \"%s\" is not of type %s",
                opt->key,
                type_to_str(opt->type)
            );
            return false;
        }

        if (!opt->callback(opt->key, dat, opt->store))
            return false;
    }
    return true;
}

bool
config_extract_string(const char *key UNUSED, toml_datum_t dat, void *store)
{
    char *str = strdup(dat.u.s);

    if (str == NULL)
    {
        log_errerror("Error allocating string for config option \"%s\"", key);
        return false;
    }

    *((char **)store) = str;

    return true;
}

bool
config_extract_int64(const char *key UNUSED, toml_datum_t dat, void *store)
{
    *((int64_t *)store) = dat.u.int64;
    return true;
}

bool
config_extract_int64_pos(const char *key, toml_datum_t dat, void *store)
{
    if (dat.u.int64 < 0)
    {
        log_error("Config option \"%s\" is not a positive integer", key);
        return false;
    }
    *((int64_t *)store) = dat.u.int64;
    return true;
}

bool
config_extract_boolean(const char *key UNUSED, toml_datum_t dat, void *store)
{
    *((bool *)store) = dat.u.boolean;
    return true;
}
