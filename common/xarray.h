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
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// IWYU pragma: end_keep

#define _XARRAY_UNUSED __attribute__((__unused__))
#define _XARRAY_MAX(a, b)                                                      \
    ({                                                                         \
        __typeof__(a) _a = (a);                                                \
        __typeof__(b) _b = (b);                                                \
        _a > _b ? _a : _b;                                                     \
    })

/*
 * "len_type" is the type to use for the size and length of the array, it must
 * be an unsigned scalar type. "initial" is the minimum initial amount of items
 * to allocate when the array size is zero. "grow_factor" is minimum factor the
 * array should grow each time when it runs out of space.
 */
#define xarray_create(type, name, len_type, initial, grow_factor)              \
    _Static_assert((len_type)(-1) > 0, "len_type must be unsigned");           \
    _Static_assert(initial >= 0, "initial must be greater than zero"); \
    _Static_assert(grow_factor >= 1.0, "grow_factor must be greater than 1.0"); \
    struct xarray_##name                                                       \
    {                                                                          \
        len_type size;                                                         \
        len_type len;                                                          \
        type    *data;                                                         \
    };                                                                         \
    _XARRAY_UNUSED static inline void xarray_init_##name(                      \
        struct xarray_##name *arr                                              \
    )                                                                          \
    {                                                                          \
        memset(arr, 0, sizeof(*arr));                                          \
    }                                                                          \
    _XARRAY_UNUSED static inline void xarray_uninit_##name(                    \
        struct xarray_##name *arr                                              \
    )                                                                          \
    {                                                                          \
        free(arr->data);                                                       \
        xarray_init_##name(arr);                                               \
    }                                                                          \
    /* Allocate enough space for "n" items for "arr". If "excess" is true,     \
     * then allocate more space than necessary. Returns true on success and    \
     * false on failure*/                                                      \
    _XARRAY_UNUSED static inline bool xarray_alloc_##name(                     \
        struct xarray_##name *arr, size_t n, bool excess                       \
    )                                                                          \
    {                                                                          \
        if (n > (__typeof__(arr->len))(-1))                                    \
            return false;                                                      \
        if (arr->size < n)                                                     \
        {                                                                      \
            size_t new_sz =                                                    \
                arr->size == 0                                                 \
                    ? _XARRAY_MAX((size_t)initial, n)                          \
                    : (excess ? _XARRAY_MAX(n, arr->size * grow_factor) : n);  \
            type *newp = realloc(arr->data, sizeof(type) * new_sz);            \
            if (newp == NULL)                                                  \
                return false;                                                  \
            arr->data = newp;                                                  \
            arr->size = (__typeof__(arr->len))new_sz;                          \
        }                                                                      \
        return true;                                                           \
    }                                                                          \
    _XARRAY_UNUSED static inline bool xarray_set_size_##name(                  \
        struct xarray_##name *arr, size_t size                                 \
    )                                                                          \
    {                                                                          \
        return xarray_alloc_##name(arr, size, false);                          \
    }                                                                          \
    _XARRAY_UNUSED static inline bool xarray_add_##name(                       \
        struct xarray_##name *arr, type val                                    \
    )                                                                          \
    {                                                                          \
        if (!xarray_alloc_##name(arr, (size_t)arr->len + 1, true))             \
            return false;                                                      \
        arr->data[arr->len++] = val;                                           \
        return true;                                                           \
    }                                                                          \
    _XARRAY_UNUSED static inline bool xarray_concat_##name(                    \
        struct xarray_##name *arr, type *vals, size_t n_vals                   \
    )                                                                          \
    {                                                                          \
        if (!xarray_alloc_##name(arr, (size_t)arr->len + n_vals, true))        \
            return false;                                                      \
        memcpy(arr->data + arr->len, vals, sizeof(type) * n_vals);             \
        arr->len += n_vals;                                                    \
        return true;                                                           \
    }                                                                          \
    _XARRAY_UNUSED static inline void xarray_clear_##name(                     \
        struct xarray_##name *arr                                              \
    )                                                                          \
    {                                                                          \
        arr->len = 0;                                                          \
    }                                                                          \
    _XARRAY_UNUSED static inline type *xarray_ptr_##name(                      \
        struct xarray_##name *arr, size_t i                                    \
    )                                                                          \
    {                                                                          \
        assert(i < arr->len);                                                  \
        return arr->data + i;                                                  \
    }                                                                          \
    _XARRAY_UNUSED static inline type xarray_val_##name(                       \
        struct xarray_##name *arr, size_t i                                    \
    )                                                                          \
    {                                                                          \
        return *xarray_ptr_##name(arr, i);                                     \
    }                                                                          \
    _XARRAY_UNUSED static inline type *xarray_data_##name(                     \
        struct xarray_##name *arr                                              \
    )                                                                          \
    {                                                                          \
        return arr->data;                                                      \
    }                                                                          \
    _XARRAY_UNUSED static inline size_t xarray_len_##name(                     \
        struct xarray_##name *arr                                              \
    )                                                                          \
    {                                                                          \
        return arr->len;                                                       \
    }                                                                          \
    _XARRAY_UNUSED static inline void xarray_del_##name(                       \
        struct xarray_##name *arr, size_t i                                    \
    )                                                                          \
    {                                                                          \
        assert(i < arr->len);                                                  \
        memmove(                                                               \
            arr->data + i,                                                     \
            arr->data + i + 1,                                                 \
            sizeof(type) * (arr->len - i - 1)                                  \
        );                                                                     \
        arr->len--;                                                            \
    }

#define xarray_foreach(name, arr, ptrv)                                        \
    _xarray_foreach(                                                           \
        name,                                                                  \
        arr,                                                                   \
        ptrv,                                                                  \
        _xarray_concat(xarray_i_, __COUNTER__),                                \
        _xarray_concat(xarray_once_, __COUNTER__),                             \
        ptr                                                                    \
    )

#define xarray_foreach_val(name, arr, value)                                   \
    _xarray_foreach(                                                           \
        name,                                                                  \
        arr,                                                                   \
        value,                                                                 \
        _xarray_concat(xarray_i_, __COUNTER__),                                \
        _xarray_concat(xarray_once_, __COUNTER__),                             \
        val                                                                    \
    )

#define _xarray_concat_(a, b) a##b
#define _xarray_concat(a, b) _xarray_concat_(a, b)
#define _xarray_foreach(name, arr, ptr, idx, once, type)                       \
    for (__typeof__((arr)->len) idx = 0;                                       \
         idx < (__typeof__((arr)->len))xarray_len_##name(arr);                 \
         idx++)                                                                \
        for (int once = ((ptr = xarray_##type##_##name((arr), idx)), 1); once; \
             once = 0)
