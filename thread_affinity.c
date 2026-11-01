/* Gives the game's busiest threads a physical core each.
 *
 * In combat no thread is saturated: the frame clock (FIFO walk and Vulkan),
 * the game's task and main threads and the audio SPU take 35-80% of a core
 * each and wait on one another, so a frame is as fast as the chain of hand-
 * offs between them. Sharing a core with a sibling hyperthread or being
 * migrated stretches each link. Measured in STG02 combat (Ryzen 5 4600H,
 * three runs each): p95 frame 22.7 ms against 24.0, 1% low 34.5 fps against
 * 31.6, average unchanged within run-to-run spread.
 *
 * A background thread looks at per-thread CPU time every few seconds and pins
 * the threads above BUSY_PCT, at most one per core and never more than
 * (cores - 2), each to both logical CPUs of its own core; every other thread
 * shares the remaining cores. With fewer than four cores it does nothing. PORT_THREAD_PIN=0 turns it off. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CORES   64
#define MAX_THREADS 512
#define BUSY_PCT    30
#define SETTLE_S    20      /* boot and menus first: let the threads appear */
#define PERIOD_S    5

static KAFFINITY s_core[MAX_CORES];
static int s_ncores;

static void read_cores(void)
{
    DWORD_PTR allowed, system;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return;
    DWORD bytes = 0;
    GetLogicalProcessorInformation(NULL, &bytes);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* info = malloc(bytes);
    if (!info) return;
    if (GetLogicalProcessorInformation(info, &bytes)) {
        const DWORD n = bytes / sizeof *info;
        for (DWORD i = 0; i < n && s_ncores < MAX_CORES; i++) {
            if (info[i].Relationship != RelationProcessorCore) continue;
            const KAFFINITY mask = info[i].ProcessorMask & allowed;
            if (mask) s_core[s_ncores++] = mask;
        }
    }
    free(info);
}

/* id and kernel+user time (100 ns units) of every thread of this process. */
static int read_threads(DWORD* tid, unsigned long long* ticks)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD self = GetCurrentProcessId();
    int n = 0;
    THREADENTRY32 e;
    e.dwSize = sizeof e;
    for (BOOL ok = Thread32First(snap, &e); ok && n < MAX_THREADS; ok = Thread32Next(snap, &e)) {
        if (e.th32OwnerProcessID != self) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, e.th32ThreadID);
        if (!h) continue;
        FILETIME created, ended, kernel, user;
        if (GetThreadTimes(h, &created, &ended, &kernel, &user)) {
            tid[n] = e.th32ThreadID;
            ticks[n++] = ((unsigned long long)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                         ((unsigned long long)user.dwHighDateTime << 32 | user.dwLowDateTime);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return n;
}

static DWORD WINAPI affinity_thread(LPVOID arg)
{
    (void)arg;
    static DWORD tid0[MAX_THREADS], tid1[MAX_THREADS], pinned[MAX_CORES];
    static unsigned long long t0[MAX_THREADS], t1[MAX_THREADS];
    const int slots = s_ncores - 2;
    Sleep(SETTLE_S * 1000);
    for (;;) {
        const int n0 = read_threads(tid0, t0);
        Sleep(PERIOD_S * 1000);
        const int n1 = read_threads(tid1, t1);
        /* The busiest threads, most first. */
        DWORD busy[MAX_THREADS];
        long long load[MAX_THREADS];
        int nbusy = 0;
        for (int i = 0; i < n1; i++) {
            unsigned long long before = 0;
            for (int j = 0; j < n0; j++) if (tid0[j] == tid1[i]) { before = t0[j]; break; }
            const long long pct = (long long)(t1[i] - before) * 100 / (10000000LL * PERIOD_S);
            if (pct < BUSY_PCT) continue;
            int k = nbusy++;
            while (k > 0 && load[k - 1] < pct) { busy[k] = busy[k - 1]; load[k] = load[k - 1]; k--; }
            busy[k] = tid1[i];
            load[k] = pct;
        }
        if (nbusy > slots) nbusy = slots;
        /* Stable cores: a thread keeps the core it has while it stays busy;
         * newly busy threads take the cores that freed up. slot[c] is the
         * thread on core c, 0 for none. */
        DWORD slot[MAX_CORES] = {0};
        int changed = 0;
        for (int c = 0; c < slots; c++)
            for (int k = 0; k < nbusy; k++) if (pinned[c] && pinned[c] == busy[k]) slot[c] = busy[k];
        for (int k = 0; k < nbusy; k++) {
            int placed = 0;
            for (int c = 0; c < slots; c++) placed |= slot[c] == busy[k];
            for (int c = 0; !placed && c < slots; c++) if (!slot[c]) { slot[c] = busy[k]; placed = 1; }
        }
        for (int c = 0; c < slots; c++) changed |= slot[c] != pinned[c];
        /* Every period, also for threads created since (a new thread inherits
         * nothing here, but one pinned earlier may have gone idle). */
        KAFFINITY rest = 0;
        for (int c = 0; c < s_ncores; c++) if (c >= slots || !slot[c]) rest |= s_core[c];
        for (int i = 0; i < n1; i++) {
            int core = -1;
            for (int c = 0; c < slots; c++) if (slot[c] && slot[c] == tid1[i]) core = c;
            HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, tid1[i]);
            if (!h) continue;
            SetThreadAffinityMask(h, core >= 0 ? s_core[core] : rest);
            CloseHandle(h);
        }
        if (changed) {
            printf("[port] %d busy threads on cores of their own (%d cores)\n", nbusy, s_ncores);
            fflush(stdout);
            memcpy(pinned, slot, sizeof pinned);
        }
    }
    return 0;
}

/* Opt-in here, PORT_THREAD_PIN=1. STG02 fighting, Ryzen 5 4600H, three pairs: no
 * clear gain over the Windows scheduler (p95 27.9 ms against 27.3, worst 5 s 37.7
 * fps against 37.3), though the runs differed in scene load. */
void port_thread_affinity_start(void)
{
    const char* e = getenv("PORT_THREAD_PIN");
    if (!e || *e != '1') return;
    read_cores();
    if (s_ncores < 4) return;
    HANDLE t = CreateThread(NULL, 256 * 1024, affinity_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t);
}
#else
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CORES   64
#define MAX_THREADS 512
#define BUSY_PCT    30
#define SETTLE_S    20      /* boot and menus first: let the threads appear */
#define PERIOD_S    5

static cpu_set_t s_core[MAX_CORES];
static int s_ncores;

static void read_cores(void)
{
    cpu_set_t allowed;
    if (sched_getaffinity(0, sizeof allowed, &allowed)) return;
    for (int cpu = 0; cpu < CPU_SETSIZE && s_ncores < MAX_CORES; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        char path[96], list[64] = "";
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
        FILE* f = fopen(path, "r");
        if (f) { if (!fgets(list, sizeof list, f)) list[0] = '\0'; fclose(f); }
        int first = atoi(list);                   /* "0-1" or "0,6": the core's first CPU */
        if (first != cpu && CPU_ISSET(first, &allowed)) continue;
        CPU_ZERO(&s_core[s_ncores]);
        for (char* p = list; *p; ) {              /* "a-b" ranges and "a,b" lists */
            char* end;
            long a = strtol(p, &end, 10), b = a;
            if (end == p) break;
            if (*end == '-') b = strtol(end + 1, &end, 10);
            for (long c = a; c <= b; c++)
                if (c < CPU_SETSIZE && CPU_ISSET(c, &allowed)) CPU_SET((int)c, &s_core[s_ncores]);
            p = *end ? end + 1 : end;
        }
        if (!CPU_COUNT(&s_core[s_ncores])) CPU_SET(cpu, &s_core[s_ncores]);
        s_ncores++;
    }
}

/* tid and utime+stime ticks of every thread of this process. */
static int read_threads(int* tid, unsigned long long* ticks)
{
    DIR* d = opendir("/proc/self/task");
    int n = 0;
    struct dirent* e;
    while (d && (e = readdir(d)) && n < MAX_THREADS) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = '\0';
        char* p = strrchr(buf, ')');
        unsigned long long ut, st;
        if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        tid[n] = atoi(e->d_name);
        ticks[n++] = ut + st;
    }
    if (d) closedir(d);
    return n;
}

static void* affinity_thread(void* arg)
{
    (void)arg;
    const long hz = sysconf(_SC_CLK_TCK);
    static int tid0[MAX_THREADS], tid1[MAX_THREADS], pinned[MAX_CORES];
    static unsigned long long t0[MAX_THREADS], t1[MAX_THREADS];
    const int slots = s_ncores - 2;
    sleep(SETTLE_S);
    for (;;) {
        const int n0 = read_threads(tid0, t0);
        sleep(PERIOD_S);
        const int n1 = read_threads(tid1, t1);
        /* The busiest threads, most first. */
        int busy[MAX_THREADS], nbusy = 0;
        long long load[MAX_THREADS];
        for (int i = 0; i < n1; i++) {
            unsigned long long before = 0;
            for (int j = 0; j < n0; j++) if (tid0[j] == tid1[i]) { before = t0[j]; break; }
            const long long pct = (long long)(t1[i] - before) * 100 / (hz * PERIOD_S);
            if (pct < BUSY_PCT) continue;
            int k = nbusy++;
            while (k > 0 && load[k - 1] < pct) { busy[k] = busy[k - 1]; load[k] = load[k - 1]; k--; }
            busy[k] = tid1[i];
            load[k] = pct;
        }
        if (nbusy > slots) nbusy = slots;
        /* Stable cores: a thread keeps the core it has while it stays busy;
         * newly busy threads take the cores that freed up. slot[c] is the
         * thread on core c, 0 for none. */
        int slot[MAX_CORES] = {0}, changed = 0;
        for (int c = 0; c < slots; c++)
            for (int k = 0; k < nbusy; k++) if (pinned[c] && pinned[c] == busy[k]) slot[c] = busy[k];
        for (int k = 0; k < nbusy; k++) {
            int placed = 0;
            for (int c = 0; c < slots; c++) placed |= slot[c] == busy[k];
            for (int c = 0; !placed && c < slots; c++) if (!slot[c]) { slot[c] = busy[k]; placed = 1; }
        }
        for (int c = 0; c < slots; c++) changed |= slot[c] != pinned[c];
        /* Every period, also for threads created since (a new thread inherits
         * its creator's mask, which may be a pinned core). Setting the mask a
         * thread already has does not move it. */
        cpu_set_t rest;
        CPU_ZERO(&rest);
        for (int c = 0; c < s_ncores; c++) if (c >= slots || !slot[c]) CPU_OR(&rest, &rest, &s_core[c]);
        for (int i = 0; i < n1; i++) {
            int core = -1;
            for (int c = 0; c < slots; c++) if (slot[c] && slot[c] == tid1[i]) core = c;
            sched_setaffinity(tid1[i], sizeof(cpu_set_t), core >= 0 ? &s_core[core] : &rest);
        }
        if (changed) {
            printf("[port] %d busy threads on cores of their own (%d cores)\n", nbusy, s_ncores);
            fflush(stdout);
            memcpy(pinned, slot, sizeof pinned);
        }
    }
    return NULL;
}

void port_thread_affinity_start(void)
{
    const char* e = getenv("PORT_THREAD_PIN");
    if (e && *e == '0') return;
    read_cores();
    if (s_ncores < 4) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&t, &attr, affinity_thread, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
}
#endif /* !_WIN32 */
