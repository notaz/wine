/*
 * Fast event implementation
 *
 * Copyright 2015 Gražvydas Ignotas
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "config.h"
#include "wine/port.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/time.h>
#include <errno.h>

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ntdll_misc.h"
#include "wine/server.h"
#include "wine/debug.h"

#include "sync_fast.h"

WINE_DEFAULT_DEBUG_CHANNEL(fast_event);

#define INDEX_TO_HANDLE(i) (HANDLE)(INT_PTR)(0x01000000 + ((i) << 2))
#define HANDLE_TO_INDEX(h) (((INT_PTR)(h) & 0x00ffffff) >> 2)

static struct {
    int futex;
    int used;
    HANDLE real_handle;
    int use_wineserver;
} fast_events[64];
#define MAX_FAST_EVENTS (sizeof(fast_events) / sizeof(fast_events[0]))

static inline void futex_setevent(int *f)
{
    int ret;

    ret = syscall(__NR_futex, f, FUTEX_WAKE, 1, NULL, NULL, 0);
    if (ret == 1)
        return;
    if (ret != 0)
        FIXME("wake %d %d\n", ret, errno);

    interlocked_cmpxchg(f, 1, 0);
}

static inline void futex_pulseevent(int *f)
{
    int ret;

    ret = syscall(__NR_futex, f, FUTEX_WAKE, 1, NULL, NULL, 0);
    if (ret == 1)
        return;
    if (ret != 0)
        FIXME("wake %d %d\n", ret, errno);

    interlocked_cmpxchg(f, 0, 1);
}

/* returns 1 if event was set, 0 on timeout, -1 on error */
static inline int futex_waitevent(int *f, LONGLONG timeout_ns)
{
    struct timespec ts, *pts = NULL;
    int ret, val;

    if (timeout_ns >= 0) {
        ts.tv_sec = timeout_ns / 1000000000;
        ts.tv_nsec = timeout_ns - ts.tv_sec * 1000000000;
        pts = &ts;
    }

    do {
        val = interlocked_cmpxchg(f, 0, 1);
        if (val == 1)
            return 1;
        if (val != 0)
            FIXME("val %d\n", val);

        ret = syscall(__NR_futex, f, FUTEX_WAIT, 0, pts, NULL, 0);
    }
    while (ret == -1 && (errno == EWOULDBLOCK || errno == EINTR));

    if (ret == 0)
        return 1;
    if (ret == -1 && errno == ETIMEDOUT)
        return 0;

    FIXME("wait %d %d\n", ret, errno);
    return -1;
}

HANDLE fast_event_create(HANDLE real_handle, BOOLEAN initial_set)
{
    HANDLE ret;
    int i;

    for (i = 0; i < MAX_FAST_EVENTS; i++)
    {
        if (!interlocked_cmpxchg(&fast_events[i].used, TRUE, FALSE))
            break;
    }
    if (i == MAX_FAST_EVENTS)
    {
        FIXME("too many event objects\n");
        return NULL;
    }

    fast_events[i].futex = initial_set ? 1 : 0;
    fast_events[i].real_handle = real_handle;
    fast_events[i].use_wineserver = FALSE;

    ret = INDEX_TO_HANDLE(i);
    TRACE("allocated %p\n", ret);

    return ret;
}

BOOLEAN fast_event_set(HANDLE *h)
{
    int i = HANDLE_TO_INDEX(*h);

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", *h);
        return FALSE;
    }

    if (fast_events[i].use_wineserver)
    {
        *h = fast_events[i].real_handle;
        return FALSE;
    }

    futex_setevent(&fast_events[i].futex);
    return TRUE;
}

BOOLEAN fast_event_pulse(HANDLE *h)
{
    int i = HANDLE_TO_INDEX(*h);

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", *h);
        return FALSE;
    }

    if (fast_events[i].use_wineserver)
    {
        *h = fast_events[i].real_handle;
        return FALSE;
    }

    futex_pulseevent(&fast_events[i].futex);
    return TRUE;
}

BOOLEAN fast_event_reset(HANDLE *h)
{
    int i = HANDLE_TO_INDEX(*h);

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", *h);
        return FALSE;
    }

    if (fast_events[i].use_wineserver)
    {
        *h = fast_events[i].real_handle;
        return FALSE;
    }

    interlocked_cmpxchg(&fast_events[i].futex, 0, 1);
    return TRUE;
}

NTSTATUS fast_event_wait(HANDLE h, const LARGE_INTEGER *timeout)
{
    int i = HANDLE_TO_INDEX(h);
    LONGLONG timeout_ns = -1ll;
    int ret;

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", h);
        return STATUS_INVALID_HANDLE;
    }

    if (fast_events[i].use_wineserver)
    {
        return STATUS_NO_EVENT_PAIR;
    }

    if (timeout != NULL)
        timeout_ns = -timeout->QuadPart * 100;

    ret = futex_waitevent(&fast_events[i].futex, timeout_ns);
    switch (ret)
    {
    case 1:
        return STATUS_WAIT_0;
    case 0:
        return STATUS_TIMEOUT;
    case -1:
    default:
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS fast_event_close(HANDLE h)
{
    int i = HANDLE_TO_INDEX(h);
    HANDLE real_handle;
    int was_used;

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", h);
        return STATUS_INVALID_HANDLE;
    }

    real_handle = fast_events[i].real_handle;
    was_used = interlocked_cmpxchg(&fast_events[i].used, FALSE, TRUE);
    if (!was_used)
    {
        FIXME("handle %p was not used?\n", h);
        return STATUS_INVALID_HANDLE;
    }

    TRACE("closed %p, real_handle %p\n", h, real_handle);
    return close_handle(real_handle);
}

HANDLE fast_event_use_wineserver(HANDLE h)
{
    int i = HANDLE_TO_INDEX(h);
    int using_wineserver;
    HANDLE real_handle;
    NTSTATUS status;

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", h);
        return h;
    }

    real_handle = fast_events[i].real_handle;
    using_wineserver = interlocked_cmpxchg(&fast_events[i].use_wineserver, TRUE, FALSE);
    if (using_wineserver)
        return real_handle;

    TRACE("handle %p switching to wineserver, state %d\n", h, fast_events[i].futex);

    /* need to ensure wineserver is up-to-date */
    SERVER_START_REQ( event_op )
    {
        req->handle = wine_server_obj_handle( real_handle );
        req->op     = fast_events[i].futex ? SET_EVENT : RESET_EVENT;
        status = wine_server_call( req );
    }
    SERVER_END_REQ;

    if (status != STATUS_SUCCESS)
        FIXME("wineserver update status %x\n", status);

    return real_handle;
}

HANDLE fast_event_get_handle(HANDLE h)
{
    int i = HANDLE_TO_INDEX(h);

    if (i >= MAX_FAST_EVENTS || !fast_events[i].used)
    {
        FIXME("bad handle %p\n", h);
        return h;
    }

    return fast_events[i].real_handle;
}

// vim:ts=4:sw=4:expandtab
