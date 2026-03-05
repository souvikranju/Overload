/*
 * overload.c - Configurable fake system overload generator for testing
 *
 * Supports: Linux x86_64, Linux ARM64, Windows x86_64, Windows ARM64
 *
 * Linux compile:
 *   gcc -Wall -Wextra -pedantic -std=c99 -pthread -O2 -o overload overload.c
 *
 * Windows cross-compile (x64, from Linux):
 *   x86_64-w64-mingw32-gcc -Wall -Wextra -pedantic -std=c99 -O2 \
 *       -o overload.exe overload.c
 *
 * Windows cross-compile (ARM64, from Linux with llvm-mingw):
 *   aarch64-w64-mingw32-gcc -Wall -Wextra -pedantic -std=c99 -O2 \
 *       -o overload-arm64.exe overload.c
 *
 * Usage:
 *   ./overload                          (CPU + RAM for 10 sec, all cores)
 *   ./overload --cpu --time 5
 *   ./overload --ram 2048 --time 10
 *   ./overload --cpu --ram 1024 --time 15 --cores 4
 */

/* =========================================================================
 * Platform detection & includes
 * ========================================================================= */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <stdint.h>
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <errno.h>
#elif defined(__APPLE__)
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <pthread.h>
#  include <unistd.h>
#  include <time.h>
#  include <errno.h>
#  include <sys/sysctl.h>
#else
#  define _POSIX_C_SOURCE 200809L
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  include <pthread.h>
#  include <unistd.h>
#  include <time.h>
#  include <errno.h>
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */
#define DEFAULT_DURATION          10   /* seconds when no args given */
#define DEFAULT_DURATION_EXPLICIT  5   /* seconds when args given but no --time */
#define RAM_AUTO_FRACTION       0.90   /* use 90% of available RAM when no MB given */
#define CHECK_INTERVAL          1000   /* check timer every this many pages */

/* =========================================================================
 * Structures
 * ========================================================================= */
typedef struct {
    int  do_cpu;    /* 1 = run CPU overload */
    int  do_ram;    /* 1 = run RAM overload */
    long ram_mb;    /* MB to allocate; 0 = auto */
    int  duration;  /* seconds */
    int  cores;     /* 0 = auto-detect */
} Config;

typedef struct {
    int    duration;   /* seconds to run */
    double start_sec;  /* start time (seconds, monotonic) */
} ThreadArg;

/* =========================================================================
 * Platform abstraction: high-resolution monotonic timer
 * ========================================================================= */
#ifdef _WIN32
static double now_sec(void)
{
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

/* =========================================================================
 * Platform abstraction: sleep milliseconds
 * ========================================================================= */
static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* =========================================================================
 * Platform abstraction: logical core count
 * ========================================================================= */
static int get_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(__APPLE__)
    int count = 1;
    size_t len = sizeof(count);
    sysctlbyname("hw.logicalcpu", &count, &len, NULL, 0);
    return count;
#else
    long sc = sysconf(_SC_NPROCESSORS_ONLN);
    return (sc > 0) ? (int)sc : 1;
#endif
}

/* =========================================================================
 * Platform abstraction: available RAM in bytes
 * ========================================================================= */
static long long get_available_ram_bytes(void)
{
#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0LL;
    return (long long)ms.ullAvailPhys;
#elif defined(__APPLE__)
    int64_t free_bytes = 0;
    size_t len = sizeof(free_bytes);
    if (sysctlbyname("hw.memsize", &free_bytes, &len, NULL, 0) == 0)
        return (long long)(free_bytes * 9 / 10); /* return 90% of total as proxy */
    return 0LL;
#else
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0LL;
    char line[256];
    long long avail_kb = 0LL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            if (sscanf(line + 13, "%lld", &avail_kb) == 1) break;
        }
    }
    fclose(f);
    return avail_kb * 1024LL;
#endif
}

/* =========================================================================
 * Platform abstraction: system page size in bytes
 * ========================================================================= */
static long get_page_size(void)
{
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (long)si.dwPageSize;
#else
    return 4096L;
#endif
}

/* =========================================================================
 * Platform abstraction: threads
 * ========================================================================= */
#ifdef _WIN32

typedef HANDLE thread_t;

static DWORD WINAPI cpu_worker_win(LPVOID arg);

static int thread_create(thread_t *t, void *arg)
{
    *t = CreateThread(NULL, 0, cpu_worker_win, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}

static void thread_join(thread_t t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else

typedef pthread_t thread_t;

static void *cpu_worker_posix(void *arg);

static int thread_create(thread_t *t, void *arg)
{
    return pthread_create(t, NULL, cpu_worker_posix, arg);
}

static void thread_join(thread_t t)
{
    pthread_join(t, NULL);
}

#endif /* _WIN32 */

/* =========================================================================
  * CPU overload worker (shared logic)
 *
 * Uses a fixed-cost busy loop: trial-divide a large candidate over a fixed
 * range of divisors so every outer iteration does the same amount of work.
 * The timer is checked only every TIMER_CHECK_ITERS outer iterations to
 * avoid the overhead of frequent now_sec() calls (especially on Windows
 * where QueryPerformanceCounter has non-trivial cost).
 * ========================================================================= */
#define DIVISOR_RANGE      50000ULL   /* inner loop iterations per outer step */
#define TIMER_CHECK_ITERS    100      /* check clock every N outer iterations (~5 ms) */

static void cpu_worker_body(ThreadArg *ta)
{
    double end_time = ta->start_sec + (double)ta->duration;

    /*
     * Use a large odd starting candidate. The inner loop always runs
     * DIVISOR_RANGE iterations regardless of whether a factor is found,
     * ensuring constant CPU load per outer iteration.
     */
    volatile unsigned long long candidate = 1000000007ULL;
    int iter = 0;

    while (1) {
        /* Fixed-cost inner loop: always iterate DIVISOR_RANGE times */
        volatile unsigned long long d;
        volatile int found = 0;
        for (d = 2ULL; d < DIVISOR_RANGE; d++) {
            if (candidate % d == 0ULL) {
                found = 1;
                /* don't break — keep iterating for constant cost */
            }
        }
        (void)found;

        /* Advance candidate to next odd number */
        candidate += 2ULL;
        if (candidate > 0xFFFFFFF0ULL) candidate = 1000000007ULL;

#if defined(__GNUC__)
        __asm__ volatile("" ::: "memory");
#elif defined(_MSC_VER)
        _ReadWriteBarrier();
#endif

        /* Check timer only every TIMER_CHECK_ITERS iterations */
        iter++;
        if (iter >= TIMER_CHECK_ITERS) {
            iter = 0;
            if (now_sec() >= end_time) break;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI cpu_worker_win(LPVOID arg)
{
    cpu_worker_body((ThreadArg *)arg);
    return 0;
}
#else
static void *cpu_worker_posix(void *arg)
{
    cpu_worker_body((ThreadArg *)arg);
    return NULL;
}
#endif

/* =========================================================================
  * RAM overload
 * ========================================================================= */
static int run_ram_overload(long ram_mb, double end_time, long *actual_mb_out)
{
    long page_sz   = get_page_size();
    long long bytes = (long long)ram_mb * 1024LL * 1024LL;

    volatile unsigned char *mem =
        (volatile unsigned char *)malloc((size_t)bytes);
    if (!mem) {
        fprintf(stderr,
            "Error: Failed to allocate %ld MB of RAM (%s).\n"
            "       Try a smaller value with --ram <MB>.\n",
            ram_mb, strerror(errno));
        return -1;
    }

    /*
     * Touch one byte per page to force physical allocation.
     * Check the clock every CHECK_INTERVAL pages so the timer is always
     * respected, even on machines with hundreds of GB of free RAM.
     */
    long long total_pages  = bytes / page_sz;
    long long pages_touched = 0LL;
    long long p;
    for (p = 0; p < total_pages; p++) {
        mem[p * page_sz] = (unsigned char)(p & 0xFF);
#if defined(__GNUC__)
        __asm__ volatile("" ::: "memory");
#elif defined(_MSC_VER)
        _ReadWriteBarrier();
#endif
        pages_touched++;
        if (p % CHECK_INTERVAL == 0 && now_sec() >= end_time) break;
    }

    *actual_mb_out = (long)(pages_touched * page_sz / (1024LL * 1024LL));

    /* Hold allocation until duration expires */
    while (now_sec() < end_time) {
        volatile unsigned char touch =
            mem[(rand() % pages_touched) * page_sz];
        (void)touch;
        sleep_ms(100);
    }

    free((void *)mem);
    return 0;
}

/* =========================================================================
 * Usage / help
 * ========================================================================= */
static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --cpu              Trigger high CPU usage across all available cores\n"
        "  --ram [MB]         Allocate specified MB of RAM\n"
        "                     (omit MB to use ~90%% of available RAM)\n"
        "  --time <secs>      Duration in seconds\n"
        "                     (default: 5 with flags, 10 with no args)\n"
        "  --cores <num>      Override CPU core count (default: all logical cores)\n"
        "\n"
        "If no arguments are given, both CPU and RAM overloads run for 10 seconds.\n"
        "\n"
        "Examples:\n"
        "  %s\n"
        "  %s --cpu --time 5\n"
        "  %s --ram 2048 --time 10\n"
        "  %s --cpu --ram 1024 --time 15 --cores 4\n",
        prog, prog, prog, prog, prog);
}

/* =========================================================================
 * Report
 * ========================================================================= */
static void print_report(const Config *cfg, int actual_cores,
                         long actual_mb, double elapsed)
{
    const char *type;
    if (cfg->do_cpu && cfg->do_ram)  type = "CPU + RAM";
    else if (cfg->do_cpu)            type = "CPU";
    else                             type = "RAM";

    printf("\n");
    printf("========================================\n");
    printf("         Overload Test Report\n");
    printf("========================================\n");
    printf("Test Type       : %s\n", type);
    printf("Duration        : %d seconds (actual: %.1f s)\n",
           cfg->duration, elapsed);
    if (cfg->do_cpu) printf("CPU Cores Used  : %d\n", actual_cores);
    if (cfg->do_ram) printf("RAM Allocated   : %ld MB\n", actual_mb);
    printf("Status          : Test completed successfully\n");
    printf("========================================\n");
}

/* =========================================================================
 * Argument parsing
 * ========================================================================= */
static int parse_args(int argc, char *argv[], Config *cfg)
{
    cfg->do_cpu   = 0;
    cfg->do_ram   = 0;
    cfg->ram_mb   = 0;
    cfg->duration = DEFAULT_DURATION_EXPLICIT;
    cfg->cores    = 0;

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cpu") == 0) {
            cfg->do_cpu = 1;

        } else if (strcmp(argv[i], "--ram") == 0) {
            cfg->do_ram = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                char *end;
                long val = strtol(argv[i + 1], &end, 10);
                if (*end == '\0' && val > 0) {
                    cfg->ram_mb = val;
                    i++;
                } else if (*end != '\0') {
                    fprintf(stderr,
                        "Error: Invalid value for --ram: '%s'\n", argv[i+1]);
                    return -1;
                }
            }

        } else if (strcmp(argv[i], "--time") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --time requires a value.\n");
                return -1;
            }
            char *end;
            long val = strtol(argv[i + 1], &end, 10);
            if (*end != '\0' || val <= 0) {
                fprintf(stderr,
                    "Error: Invalid value for --time: '%s'\n", argv[i+1]);
                return -1;
            }
            cfg->duration = (int)val;
            i++;

        } else if (strcmp(argv[i], "--cores") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --cores requires a value.\n");
                return -1;
            }
            char *end;
            long val = strtol(argv[i + 1], &end, 10);
            if (*end != '\0' || val <= 0) {
                fprintf(stderr,
                    "Error: Invalid value for --cores: '%s'\n", argv[i+1]);
                return -1;
            }
            cfg->cores = (int)val;
            i++;

        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h")     == 0) {
            return -1;

        } else {
            fprintf(stderr, "Error: Unknown argument: '%s'\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[])
{
    Config cfg;

    if (argc == 1) {
        cfg.do_cpu   = 1;
        cfg.do_ram   = 1;
        cfg.ram_mb   = 0;
        cfg.duration = DEFAULT_DURATION;
        cfg.cores    = 0;
    } else {
        if (parse_args(argc, argv, &cfg) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (!cfg.do_cpu && !cfg.do_ram) {
            cfg.do_cpu = 1;
            cfg.do_ram = 1;
        }
    }

    /* ---- Resolve core count ---- */
    int phys_cores = get_cpu_count();
    int num_cores  = cfg.cores;
    if (num_cores <= 0) {
        num_cores = phys_cores;
    } else if (num_cores > phys_cores) {
        fprintf(stderr,
            "Warning: --cores %d exceeds available logical cores (%d). "
            "Extra threads will be time-sliced by the OS scheduler.\n",
            num_cores, phys_cores);
    }

    /* ---- Resolve RAM size ---- */
    long long avail_ram_bytes = cfg.do_ram ? get_available_ram_bytes() : 0LL;
    long ram_mb = cfg.ram_mb;

    if (cfg.do_ram && ram_mb == 0) {
        if (avail_ram_bytes <= 0) {
            fprintf(stderr,
                "Warning: Could not detect available RAM. "
                "Defaulting to 512 MB.\n");
            ram_mb = 512;
        } else {
            ram_mb = (long)((double)avail_ram_bytes
                            * RAM_AUTO_FRACTION / (1024.0 * 1024.0));
            if (ram_mb < 1) ram_mb = 1;
        }
    } else if (cfg.do_ram && ram_mb > 0 && avail_ram_bytes > 0) {
        long long requested_bytes = (long long)ram_mb * 1024LL * 1024LL;
        long long avail_mb        = avail_ram_bytes / (1024LL * 1024LL);
        if (requested_bytes > avail_ram_bytes) {
            fprintf(stderr,
                "Error: Requested %ld MB exceeds available RAM (%lld MB).\n"
                "       Use --ram %lld or less to avoid an out-of-memory kill.\n",
                ram_mb, avail_mb, (long long)(avail_mb * 9 / 10));
            return 1;
        }
    }

    /* ---- Print startup info ---- */
    printf("Starting overload...\n");
    if (cfg.do_cpu)
        printf("  CPU overload : %d core(s) for %d second(s)\n",
               num_cores, cfg.duration);
    if (cfg.do_ram)
        printf("  RAM overload : %ld MB for %d second(s)\n",
               ram_mb, cfg.duration);
    printf("\n");

    /* ---- Record start time ---- */
    double t_start = now_sec();
    double t_end   = t_start + (double)cfg.duration;

    ThreadArg thread_arg;
    thread_arg.duration  = cfg.duration;
    thread_arg.start_sec = t_start;

    /* ---- Spawn CPU threads ---- */
    thread_t *threads    = NULL;
    int       actual_cores = 0;

    if (cfg.do_cpu) {
        threads = (thread_t *)malloc((size_t)num_cores * sizeof(thread_t));
        if (!threads) {
            fprintf(stderr, "Error: Failed to allocate thread array.\n");
            return 1;
        }
        int t;
        for (t = 0; t < num_cores; t++) {
            if (thread_create(&threads[t], &thread_arg) != 0) {
                fprintf(stderr,
                    "Error: Failed to create thread %d.\n", t);
                int j;
                for (j = 0; j < t; j++) thread_join(threads[j]);
                free(threads);
                return 1;
            }
            actual_cores++;
        }
    }

    /* ---- RAM overload (main thread) ---- */
    long actual_mb = 0;

    if (cfg.do_ram) {
        if (run_ram_overload(ram_mb, t_end, &actual_mb) != 0) {
            if (cfg.do_cpu && threads) {
                int t;
                for (t = 0; t < actual_cores; t++) thread_join(threads[t]);
                free(threads);
            }
            return 1;
        }
    } else {
        /* CPU-only: main thread sleeps for the duration */
        while (now_sec() < t_end) sleep_ms(100);
    }

    /* ---- Join CPU threads ---- */
    if (cfg.do_cpu && threads) {
        int t;
        for (t = 0; t < actual_cores; t++) thread_join(threads[t]);
        free(threads);
    }

    double elapsed = now_sec() - t_start;
    print_report(&cfg, actual_cores, actual_mb, elapsed);
    return 0;
}
