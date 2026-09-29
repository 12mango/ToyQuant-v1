/*
 * Counts the libc calls a function level profile cannot see. gprof samples the program's own text, so
 * anything resolved to a shared library looks like a leaf with no body: the call is visible, its cost
 * is not. This interposes the few calls that matter for a market data reader and reports call counts,
 * byte totals and a size histogram.
 *
 * Counting is deliberately cheap. A clock read per call would cost more than some of the calls being
 * measured, so per-call cost is left to the isolation benches and this answers a different question:
 * how many calls, and what shape are they.
 *
 * Build:  gcc -O2 -shared -fPIC -o /tmp/count_libc_calls.so tools/count_libc_calls.c -ldl
 * Use:    LD_PRELOAD=/tmp/count_libc_calls.so <binary> ... 2>&1 | grep SHIM
 *
 * Result on the L2 workload, per event over 1,303,131 events: 22 memchr and 9 memcpy calls, all short,
 * and 279 read syscalls for the whole run. That ruled out I/O and hidden library calls as the missing
 * cost that the stage table had suggested were large.
 *
 * <string.h> is not included on purpose: with a fortified libc it rewrites memchr through _Generic,
 * which collides with defining an interposer of the same name. Numbers are formatted by hand for the
 * same reason, so nothing here can recurse through the functions it wraps.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

extern int atexit(void (*)(void));

typedef void* (*MemchrFn)(const void*, int, size_t);
typedef void* (*MemcpyFn)(void*, const void*, size_t);
typedef ssize_t (*ReadFn)(int, void*, size_t);

static MemchrFn next_memchr;
static MemcpyFn next_memcpy;
static MemcpyFn next_memmove;
static ReadFn next_read;
static unsigned long long n_memchr, b_memchr, max_memchr;
static unsigned long long n_memcpy, b_memcpy, max_memcpy;
static unsigned long long n_memmove, b_memmove;
static unsigned long long n_read, b_read;
/* Size buckets: <=16, <=64, <=256, <=4096, <=65536, <=1M, larger. */
static unsigned long long mc_calls[7], mc_bytes[7];
static unsigned long long cp_calls[7], cp_bytes[7];
static int ready;

static int bucket(size_t n)
{
    if (n <= 16) return 0;
    if (n <= 64) return 1;
    if (n <= 256) return 2;
    if (n <= 4096) return 3;
    if (n <= 65536) return 4;
    if (n <= 1048576) return 5;
    return 6;
}

static int put_text(char* out, const char* text)
{
    int n = 0;
    while (text[n] != '\0') ++n;
    for (int i = 0; i < n; ++i) out[i] = text[i];
    return n;
}

static int put_u64(char* out, unsigned long long value)
{
    char digits[24];
    int n = 0;
    do
    {
        digits[n++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    for (int i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    return n;
}

static void histogram(char* buf, int* at, const char* name, unsigned long long* calls,
                      unsigned long long* bytes)
{
    static const char* labels[7] = {"<=16", "<=64", "<=256", "<=4096", "<=64K", "<=1M", ">1M"};
    for (int i = 0; i < 7; ++i)
    {
        if (calls[i] == 0) continue;
        *at += put_text(buf + *at, "[SHIM]   ");
        *at += put_text(buf + *at, name);
        *at += put_text(buf + *at, " ");
        *at += put_text(buf + *at, labels[i]);
        *at += put_text(buf + *at, " calls=");
        *at += put_u64(buf + *at, calls[i]);
        *at += put_text(buf + *at, " bytes=");
        *at += put_u64(buf + *at, bytes[i]);
        *at += put_text(buf + *at, "\n");
    }
}

static void report(void)
{
    char buf[2048];
    int n = 0;
    n += put_text(buf + n, "[SHIM] memchr  calls=");
    n += put_u64(buf + n, n_memchr);
    n += put_text(buf + n, " requested-bytes=");
    n += put_u64(buf + n, b_memchr);
    n += put_text(buf + n, " max=");
    n += put_u64(buf + n, max_memchr);
    n += put_text(buf + n, "\n");
    histogram(buf, &n, "memchr", mc_calls, mc_bytes);
    n += put_text(buf + n, "[SHIM] memcpy  calls=");
    n += put_u64(buf + n, n_memcpy);
    n += put_text(buf + n, " bytes=");
    n += put_u64(buf + n, b_memcpy);
    n += put_text(buf + n, " max=");
    n += put_u64(buf + n, max_memcpy);
    n += put_text(buf + n, "\n");
    histogram(buf, &n, "memcpy", cp_calls, cp_bytes);
    n += put_text(buf + n, "[SHIM] memmove calls=");
    n += put_u64(buf + n, n_memmove);
    n += put_text(buf + n, " bytes=");
    n += put_u64(buf + n, b_memmove);
    n += put_text(buf + n, "\n[SHIM] read    calls=");
    n += put_u64(buf + n, n_read);
    n += put_text(buf + n, " bytes=");
    n += put_u64(buf + n, b_read);
    n += put_text(buf + n, "\n");
    const ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;
}

/* ready is set before dlsym, so a call arriving while dlsym runs takes the fallback path instead of
 * recursing. */
static void init(void)
{
    if (ready) return;
    ready = 1;
    next_memchr = (MemchrFn)dlsym(RTLD_NEXT, "memchr");
    next_memcpy = (MemcpyFn)dlsym(RTLD_NEXT, "memcpy");
    next_memmove = (MemcpyFn)dlsym(RTLD_NEXT, "memmove");
    next_read = (ReadFn)dlsym(RTLD_NEXT, "read");
    atexit(report);
}

void* memchr(const void* s, int c, size_t n)
{
    init();
    ++n_memchr;
    b_memchr += n;
    if (n > max_memchr) max_memchr = n;
    const int k = bucket(n);
    ++mc_calls[k];
    mc_bytes[k] += n;
    if (next_memchr == NULL)
    {
        const unsigned char* p = (const unsigned char*)s;
        for (size_t i = 0; i < n; ++i)
            if (p[i] == (unsigned char)c) return (void*)(p + i);
        return NULL;
    }
    return next_memchr(s, c, n);
}

void* memcpy(void* dst, const void* src, size_t n)
{
    init();
    ++n_memcpy;
    b_memcpy += n;
    if (n > max_memcpy) max_memcpy = n;
    const int k = bucket(n);
    ++cp_calls[k];
    cp_bytes[k] += n;
    if (next_memcpy == NULL)
    {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* p = (const unsigned char*)src;
        for (size_t i = 0; i < n; ++i) d[i] = p[i];
        return dst;
    }
    return next_memcpy(dst, src, n);
}

void* memmove(void* dst, const void* src, size_t n)
{
    init();
    ++n_memmove;
    b_memmove += n;
    if (next_memmove == NULL) return memcpy(dst, src, n);
    return next_memmove(dst, src, n);
}

ssize_t read(int fd, void* buf, size_t n)
{
    init();
    const ssize_t got = (next_read == NULL) ? (ssize_t)-1 : next_read(fd, buf, n);
    ++n_read;
    if (got > 0) b_read += (unsigned long long)got;
    return got;
}
