
static inline BOOL is_fast_event_handle(HANDLE h)
{
    return ((INT_PTR)h & 0xff000000) == 0x01000000;
}

HANDLE   fast_event_create(HANDLE real_handle, BOOLEAN initial_set);
BOOLEAN  fast_event_set(HANDLE *h);
BOOLEAN  fast_event_pulse(HANDLE *h);
BOOLEAN  fast_event_reset(HANDLE *h);
NTSTATUS fast_event_wait(HANDLE h, const LARGE_INTEGER *timeout);
NTSTATUS fast_event_close(HANDLE h);
HANDLE   fast_event_use_wineserver(HANDLE h);
HANDLE   fast_event_get_handle(HANDLE h);
