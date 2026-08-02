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

#include "config.h"
#include "common/config_util.h"
#include "common/log.h"
#include "common/util.h"

static bool
extract_configured_seats(const char *key, toml_datum_t dat, void *store)
{
    struct config             *config = store;
    struct xarray_config_seat *arr = &config->configured_seats;

    if (dat.u.tab.size == 0)
        return true;

    if (!xarray_set_size_config_seat(arr, dat.u.tab.size))
    {
        log_errerror("Error allocating array for configured seats");
        return false;
    }

    for (int32_t i = 0; i < dat.u.tab.size; i++)
    {
        const char  *seatname = dat.u.tab.key[i];
        toml_datum_t t_seat = dat.u.tab.value[i];

        if (t_seat.type != TOML_TABLE)
        {
            log_error("expected table for \"%s.%s\"", key, seatname);
            return false;
        }

        struct config_seat seat = {
            .name = NULL, .regular = config->regular, .primary = config->primary
        };

        seat.name = strdup(seatname);
        if (seat.name == NULL)
        {
            log_errerror("Error allocating seat name for config");
            return false;
        }

        const struct config_option opts[] = {
            CONFIG_BOOLEAN("regular", &seat.regular),
            CONFIG_BOOLEAN("primary", &seat.primary)
        };

        if (!config_extract(t_seat, opts, N_ELEMENTS(opts)))
            return false;

        xarray_add_config_seat(arr, seat);
    }

    return true;
}

static bool
extract_pattern_array(const char *key, toml_datum_t dat, void *store)
{
    struct xarray_regex *arr = store;

    if (dat.u.arr.size == 0)
        return true;

    if (!xarray_set_size_regex(arr, dat.u.tab.size))
    {
        log_errerror("Error allocating array for \"%s\"", key);
        return false;
    }

    for (int32_t i = 0; i < dat.u.arr.size; i++)
    {
        toml_datum_t t_pattern = dat.u.arr.elem[i];

        if (t_pattern.type != TOML_STRING)
        {
            log_error(
                "Expected string for pattern in \"%s[%d]\" in config", key, i
            );
            return false;
        }

        regex_t re;
        int     res = regcomp(&re, t_pattern.u.s, REG_EXTENDED | REG_NOSUB);

        if (res != 0)
        {
            static char errbuf[128];

            regerror(res, &re, errbuf, 128);
            log_error(
                "Invalid pattern '%s' in config: %s", t_pattern.u.s, errbuf
            );
            return false;
        }

        xarray_add_regex(arr, re);
    }

    return true;
}

static bool
extract_dedup(const char *key UNUSED, toml_datum_t dat, void *store)
{
    enum config_dedup *setting = store;

    if (strcmp(dat.u.s, "none") == 0)
        *setting = DEDUP_NONE;
    else if (strcmp(dat.u.s, "prev") == 0)
        *setting = DEDUP_PREV;
    else if (strcmp(dat.u.s, "all") == 0)
        *setting = DEDUP_ALL;
    else
    {
        log_error("Unknown dedup setting \"%s\" in config", dat.u.s);
        return false;
    }
    return true;
}

bool
config_init(struct config *config, const char *file)
{
    toml_result_t result;

    if (!config_parse(file, &result))
        return false;

    *config = (struct config){
        .max_entries = 100,
        .max_size = 20000000, // 20 MB
        .persist = true,
        .regular = true,
        .primary = true,
        .set_on_startup = true,
        .dedup = DEDUP_ALL
    };

    xarray_init_config_seat(&config->configured_seats);
    xarray_init_regex(&config->allowed_mime_types);
    xarray_init_regex(&config->blocked_mime_types);

    const struct config_option opts[] = {
        CONFIG_INT64_POS("daemon.max_entries", &config->max_entries),
        CONFIG_INT64_POS("daemon.max_size", &config->max_size),
        CONFIG_BOOLEAN("daemon.persist", &config->persist),
        CONFIG_BOOLEAN("daemon.regular", &config->regular),
        CONFIG_BOOLEAN("daemon.primary", &config->primary),
        CONFIG_TABLE("daemon.seats", config, extract_configured_seats),
        CONFIG_BOOLEAN("daemon.set_on_startup", &config->set_on_startup),
        CONFIG_STRING_CUSTOM("daemon.dedup", &config->dedup, extract_dedup),
        CONFIG_ARRAY(
            "daemon.mime_types.allowed",
            &config->allowed_mime_types,
            extract_pattern_array
        ),
        CONFIG_ARRAY(
            "daemon.mime_types.blocked",
            &config->blocked_mime_types,
            extract_pattern_array
        )
    };

    bool ret = config_extract(result.toptab, opts, N_ELEMENTS(opts));

    toml_free(result);

    if (!ret)
    {
        config_uninit(config);
        return false;
    }

    return true;
}

void
config_uninit(struct config *config)
{
    struct config_seat *config_seat;

    xarray_foreach(config_seat, &config->configured_seats, config_seat)
        free(config_seat->name);
    xarray_uninit_config_seat(&config->configured_seats);

    regex_t *reg;

    xarray_foreach(regex, &config->allowed_mime_types, reg) regfree(reg);
    xarray_uninit_regex(&config->allowed_mime_types);

    xarray_foreach(regex, &config->blocked_mime_types, reg) regfree(reg);
    xarray_uninit_regex(&config->blocked_mime_types);
}
