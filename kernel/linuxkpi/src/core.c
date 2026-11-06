#include <lkpi/all.h>
#include "../host.h"

/* kernel/linuxkpi: printing, assertions and strings, as Linux has them, on our kernel's log and formatting. */

/* ---- printing ---- */

int vprintk(const char *fmt, va_list ap) {
    char buf[512];
    const char *f = fmt;
    if (f[0] == '\001' && f[1]) /* KERN_ERR and friends: the level is not shown */
        f += 2;
    int n = kpi_host_vsnprintf(buf, sizeof buf, f, ap);
    size_t len = n < 0 ? 0 : (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    kpi_host_log("nvidia: ", 8);
    kpi_host_log(buf, len);
    if (len && buf[len - 1] != '\n')
        kpi_host_log("\n", 1);
    return n;
}

int printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

int dev_printk_emit(int level, const struct device *d, const char *fmt, ...) {
    (void)level, (void)d;
    va_list ap;
    va_start(ap, fmt);
    int n = vprintk(fmt, ap);
    va_end(ap);
    return n;
}

void lkpi_bug(const char *file, int line) {
    char m[200];
    snprintf(m, sizeof m, "nvidia: BUG at %s:%d", file, line);
    kpi_host_panic(m);
}

void lkpi_warn(const char *file, int line, const char *what) { printk("WARNING at %s:%d: %s\n", file, line, what); }
void dump_stack(void) { kpi_host_backtrace(); }

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) { return kpi_host_vsnprintf(buf, size, fmt, ap); }

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int scnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (!size)
        return 0;
    return n < 0 ? 0 : (size_t)n >= size ? (int)size - 1 : n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, 4096, fmt, ap);
    va_end(ap);
    return n;
}

char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap) {
    va_list again;
    va_copy(again, ap);
    int n = vsnprintf(NULL, 0, fmt, again);
    va_end(again);
    if (n < 0)
        return NULL;
    char *s = kmalloc((size_t)n + 1, gfp);
    if (s)
        vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

char *kasprintf(gfp_t gfp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = kvasprintf(gfp, fmt, ap);
    va_end(ap);
    return s;
}

/* ---- numbers from text ---- */

static int digit(char c, unsigned int base) {
    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
    return d < (int)base ? d : -1;
}

unsigned long simple_strtoul(const char *s, char **end, unsigned int base) {
    while (*s == ' ' || *s == '\t')
        s++;
    if (!base) {
        base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            base = 16, s += 2;
        else if (s[0] == '0')
            base = 8;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    unsigned long v = 0;
    for (int d; (d = digit(*s, base)) >= 0; s++)
        v = v * base + (unsigned long)d;
    if (end)
        *end = (char *)s;
    return v;
}

int kstrtoul(const char *s, unsigned int base, unsigned long *res) {
    char *e;
    if (!s || !*s)
        return -EINVAL;
    unsigned long v = simple_strtoul(s, &e, base);
    if (e == s || (*e && !(*e == '\n' && !e[1])))
        return -EINVAL;
    *res = v;
    return 0;
}

int kstrtouint(const char *s, unsigned int base, unsigned int *res) {
    unsigned long v;
    int r = kstrtoul(s, base, &v);
    if (r)
        return r;
    if (v > UINT_MAX)
        return -ERANGE;
    *res = (unsigned int)v;
    return 0;
}

int kstrtoint(const char *s, unsigned int base, int *res) {
    bool neg = s && *s == '-';
    unsigned long v;
    int r = kstrtoul(neg ? s + 1 : s, base, &v);
    if (r)
        return r;
    if (v > (unsigned long)INT_MAX + (neg ? 1 : 0))
        return -ERANGE;
    *res = neg ? -(int)v : (int)v;
    return 0;
}

/* %d %u %x %s %c %n and widths: what the driver's registry and option parsing use */
int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = 0;
    const char *start = s;
    while (*fmt) {
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n')
                s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt)
                break;
            s++, fmt++;
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int longs = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
            longs += *fmt++ == 'l';
        char c = *fmt++;
        if (c == 'n') {
            *va_arg(ap, int *) = (int)(s - start);
            continue;
        }
        if (c == 'c') {
            if (!*s)
                break;
            *va_arg(ap, char *) = *s++;
            n++;
            continue;
        }
        while (*s == ' ' || *s == '\t')
            s++;
        if (c == 's') {
            char *out = va_arg(ap, char *);
            int k = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n' && (!width || k < width))
                out[k++] = *s++;
            out[k] = 0;
            if (!k)
                break;
            n++;
            continue;
        }
        unsigned int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
        bool neg = (c == 'd' || c == 'i') && *s == '-';
        if (neg)
            s++;
        char *e;
        unsigned long v = simple_strtoul(s, &e, base);
        if (e == s)
            break;
        s = e;
        long sv = neg ? -(long)v : (long)v;
        if (longs >= 1)
            *va_arg(ap, long *) = sv;
        else
            *va_arg(ap, int *) = (int)sv;
        n++;
    }
    va_end(ap);
    return n;
}

/* ---- strings (memcpy, memset, memcmp, memmove, strlen, strcmp and strncmp are our kernel's own) ---- */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    return NULL;
}

size_t strnlen(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i])
        i++;
    return i;
}


int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) { return strncasecmp(a, b, (size_t)-1); }

char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n) {
    size_t i = 0;
    for (; i < n && s[i]; i++)
        d[i] = s[i];
    for (; i < n; i++)
        d[i] = 0;
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}

ssize_t sized_strscpy(char *d, const char *s, size_t n) {
    if (!n)
        return -E2BIG;
    size_t l = strnlen(s, n);
    if (l == n) {
        memcpy(d, s, n - 1);
        d[n - 1] = 0;
        return -E2BIG;
    }
    memcpy(d, s, l + 1);
    return (ssize_t)l;
}

ssize_t strscpy(char *d, const char *s, size_t n) { return sized_strscpy(d, s, n); }

char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n) {
    char *e = d + strlen(d);
    size_t i = 0;
    for (; i < n && s[i]; i++)
        e[i] = s[i];
    e[i] = 0;
    return d;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    if (!l)
        return (char *)h;
    for (; *h; h++)
        if (!strncmp(h, n, l))
            return (char *)h;
    return NULL;
}

char *strsep(char **sp, const char *delim) {
    char *s = *sp;
    if (!s)
        return NULL;
    for (char *p = s; *p; p++)
        if (strchr(delim, *p)) {
            *p = 0;
            *sp = p + 1;
            return s;
        }
    *sp = NULL;
    return s;
}

char *skip_spaces(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return (char *)s;
}

char *strim(char *s) {
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\n' || s[l - 1] == '\r'))
        s[--l] = 0;
    return skip_spaces(s);
}

char *kstrndup(const char *s, size_t n, gfp_t gfp) {
    if (!s)
        return NULL;
    size_t l = strnlen(s, n);
    char *d = kmalloc(l + 1, gfp);
    if (d) {
        memcpy(d, s, l);
        d[l] = 0;
    }
    return d;
}

char *kstrdup(const char *s, gfp_t gfp) { return kstrndup(s, (size_t)-1, gfp); }

void *kmemdup(const void *s, size_t n, gfp_t gfp) {
    void *d = kmalloc(n, gfp);
    if (d)
        memcpy(d, s, n);
    return d;
}

int sysfs_streq(const char *a, const char *b) {
    while (*a && *a == *b)
        a++, b++;
    if (*a == *b)
        return 1;
    if (!*a && *b == '\n' && !b[1])
        return 1;
    if (!*b && *a == '\n' && !a[1])
        return 1;
    return 0;
}

unsigned int full_name_hash(const void *salt, const char *name, unsigned int len) {
    unsigned int h = (unsigned int)(uintptr_t)salt * 2654435761u;
    for (unsigned int i = 0; i < len; i++)
        h = (h ^ (unsigned char)name[i]) * 16777619u;
    return h;
}

/* ---- bitmaps ---- */

unsigned long find_next_bit(const unsigned long *a, unsigned long size, unsigned long off) {
    for (unsigned long i = off; i < size; i++)
        if (test_bit((long)i, a))
            return i;
    return size;
}

unsigned long find_next_zero_bit(const unsigned long *a, unsigned long size, unsigned long off) {
    for (unsigned long i = off; i < size; i++)
        if (!test_bit((long)i, a))
            return i;
    return size;
}

unsigned long find_first_bit(const unsigned long *a, unsigned long size) { return find_next_bit(a, size, 0); }
unsigned long find_first_zero_bit(const unsigned long *a, unsigned long size) { return find_next_zero_bit(a, size, 0); }

int bitmap_weight(const unsigned long *a, unsigned int n) {
    int w = 0;
    for (unsigned int i = 0; i < n; i++)
        w += test_bit((long)i, a);
    return w;
}

void bitmap_set(unsigned long *a, unsigned int start, unsigned int n) {
    for (unsigned int i = start; i < start + n; i++)
        set_bit((long)i, a);
}

void bitmap_clear(unsigned long *a, unsigned int start, unsigned int n) {
    for (unsigned int i = start; i < start + n; i++)
        clear_bit((long)i, a);
}

bool bitmap_empty(const unsigned long *a, unsigned int n) { return find_first_bit(a, n) >= n; }
bool bitmap_full(const unsigned long *a, unsigned int n) { return find_first_zero_bit(a, n) >= n; }
void bitmap_copy(unsigned long *d, const unsigned long *s, unsigned int n) { memcpy(d, s, BITS_TO_LONGS(n) * sizeof(long)); }

void bitmap_or(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n) {
    for (unsigned int i = 0; i < BITS_TO_LONGS(n); i++)
        d[i] = a[i] | b[i];
}

int bitmap_and(unsigned long *d, const unsigned long *a, const unsigned long *b, unsigned int n) {
    unsigned long any = 0;
    for (unsigned int i = 0; i < BITS_TO_LONGS(n); i++)
        any |= d[i] = a[i] & b[i];
    return any != 0;
}

unsigned long bitmap_find_next_zero_area(unsigned long *map, unsigned long size, unsigned long start, unsigned int nr,
                                         unsigned long align_mask) {
    for (unsigned long i = (start + align_mask) & ~align_mask; i + nr <= size; i = (i + 1 + align_mask) & ~align_mask) {
        unsigned long e = find_next_bit(map, i + nr, i);
        if (e >= i + nr)
            return i;
        i = e;
    }
    return size;
}

unsigned long *bitmap_zalloc(unsigned int n, gfp_t gfp) { return kzalloc(BITS_TO_LONGS(n) * sizeof(long), gfp); }
void bitmap_free(const unsigned long *b) { kfree(b); }

/* ---- random numbers ---- */

void get_random_bytes(void *buf, size_t n) { kpi_host_random(buf, n); }

u32 get_random_u32(void) {
    u32 v;
    kpi_host_random(&v, sizeof v);
    return v;
}
