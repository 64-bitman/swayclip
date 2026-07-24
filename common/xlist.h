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

// IWYU pragma: begin_keep
#include <stdbool.h>
#include <stddef.h>
// IWYU pragma: end_keep

#define _XLIST_UNUSED __attribute__((__unused__))

#define xlist_declare(name)                                                    \
    struct xlist_##name                                                        \
    {                                                                          \
        struct xlist_##name *next;                                             \
        struct xlist_##name *prev;                                             \
    }

#define xlist_define(name, type, link_name)                                    \
    _XLIST_UNUSED static inline void xlist_init_##name(                        \
        struct xlist_##name *link                                              \
    )                                                                          \
    {                                                                          \
        link->next = link->prev = link;                                        \
    }                                                                          \
    /* Insert "node" after "list". Note that "node" link does not have to be   \
     * initialized*/                                                           \
    _XLIST_UNUSED static inline void xlist_insert_after_##name(                \
        struct xlist_##name *list, type *node                                  \
    )                                                                          \
    {                                                                          \
        struct xlist_##name *link = &node->link_name;                          \
        link->next = list->next;                                               \
        list->next->prev = link;                                               \
        link->prev = list;                                                     \
        list->next = link;                                                     \
    }                                                                          \
    _XLIST_UNUSED static inline void xlist_unlink_##name(type *node)           \
    {                                                                          \
        struct xlist_##name *link = &node->link_name;                          \
        link->prev->next = link->next;                                         \
        link->next->prev = link->prev;                                         \
        link->next = link->prev = link;                                        \
    }                                                                          \
    _XLIST_UNUSED static inline bool xlist_empty_##name(                       \
        struct xlist_##name *list                                              \
    )                                                                          \
    {                                                                          \
        return list->next == list;                                             \
    }                                                                          \
    _XLIST_UNUSED static inline type *xlist_ptr_##name(                        \
        struct xlist_##name *link                                              \
    )                                                                          \
    {                                                                          \
        return ((type *)(void *)((uint8_t *)(link) -                           \
                                 offsetof(type, link_name)));                  \
    }

#define xlist_foreach(name, list, pos)                                         \
    for (struct xlist_##name *__xlist_it_##name = (list)->next;                \
         __xlist_it_##name != (list) &&                                        \
         ((pos = xlist_ptr_##name(__xlist_it_##name)), 1);                     \
         __xlist_it_##name = __xlist_it_##name->next)

/*
 * Safe to remove current list link.
 */
#define xlist_foreach_safe(name, list, pos)                                    \
    for (struct xlist_##name *__xlist_it_##name = (list)->next,                \
                             *__xlist_tmp_##name = __xlist_it_##name->next;    \
         __xlist_it_##name != (list) &&                                        \
         ((pos = xlist_ptr_##name(__xlist_it_##name)), 1);                     \
         __xlist_it_##name = __xlist_tmp_##name,                               \
                             __xlist_tmp_##name = __xlist_it_##name->next)
