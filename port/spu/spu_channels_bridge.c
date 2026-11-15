/* The SDK's SPU channel/MFC layer, unchanged except that its getenv() calls
 * answer from a cache. Several sit on paths every channel read or write takes
 * (mfc_for looks up SPU_PCSAMPLE each time while it is unset), and with the
 * audio task translated they were a fifth of that thread's time. The
 * environment does not change while the game runs, and every lookup here
 * passes a string literal, so the literal's address is the key. */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static const char* port_getenv_once(const char* name);
#define getenv port_getenv_once
#include PORT_SPU_CHANNELS_SOURCE
#undef getenv

static const char* port_getenv_once(const char* name)
{
    enum { N = 128 };
    static struct { const char* name; const char* value; } s_cache[N];
    static _Atomic unsigned s_count;
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    unsigned n = s_count;
    for (unsigned i = 0; i < n; i++)
        if (s_cache[i].name == name) return s_cache[i].value;
    const char* value = getenv(name);
    pthread_mutex_lock(&s_lock);
    n = s_count;
    if (n < N) {
        s_cache[n].name = name;
        s_cache[n].value = value;
        s_count = n + 1;          /* publish after the entry is filled */
    }
    pthread_mutex_unlock(&s_lock);
    return value;
}
