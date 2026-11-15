/* The SDK's Win32 compatibility layer, with WaitForSingleObject reading the
 * clock only when the answer depends on it.
 *
 * Every guest sys_lwmutex_lock and trylock starts with a zero-timeout wait on
 * the mutex's semaphore. The SDK's version took the monotonic clock twice per
 * call (the deadline, then "now" for the signalled check), although only a
 * waitable timer's state depends on the time and a zero or infinite wait has
 * no deadline. In crowded STG02 combat those reads were about 6.5% of the
 * busy PPU thread's samples, the thread that limits the frame rate there.
 * Everything else, including the locking and the timer semantics, is the
 * SDK's code compiled here unchanged. */
#define WaitForSingleObject port_sdk_WaitForSingleObject
#include PORT_WIN32_COMPAT_SOURCE
#undef WaitForSingleObject

DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    ps3_obj* o = obj_of(h);
    if (!o) { SetLastError(ERROR_INVALID_HANDLE); return WAIT_FAILED; }
    const int timed = o->kind == OBJ_TIMER;   /* fixed at creation */
    unsigned long long deadline = (ms == 0 || ms == INFINITE) ? 0ull : deadline_from_ms(ms);
    DWORD result = WAIT_TIMEOUT;

    pthread_mutex_lock(&s_lock);
    o->refs++;                   /* the object must outlive the wait even if closed meanwhile */
    for (;;) {
        unsigned long long now = timed ? mono_ns() : 0ull;
        if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; break; }
        if (ms == 0) break;
        if (wait_until_locked(&o->cv, deadline, obj_next_due_locked(o)) == ETIMEDOUT) {
            /* One last look: the state may have changed exactly at the deadline. */
            now = timed ? mono_ns() : 0ull;
            if (obj_signaled_locked(o, now)) { obj_consume_locked(o, now); result = WAIT_OBJECT_0; }
            break;
        }
    }
    obj_release_locked(o);
    pthread_mutex_unlock(&s_lock);
    if (result == WAIT_TIMEOUT) SetLastError(ERROR_TIMEOUT);
    return result;
}
