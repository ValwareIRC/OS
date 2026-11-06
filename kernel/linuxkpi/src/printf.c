#include <lkpi/all.h>

/* kernel/linuxkpi: vsnprintf as Linux's does it, for NVIDIA's messages and the strings it builds: the flags
 * (- + space # 0), width and precision (either may be '*'), the lengths hh h l ll z t j L, the conversions
 * d i u o x X c s p n %, and Linux's own %p extensions (%px %pK %pa[dp] %pU[bBlL] %pM %pm %pI4 %pS...). Our
 * kernel's own printf is smaller, and is not used for these. */

struct out { char *buf; size_t size, len; };

static void put(struct out *o, char c) {
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void puts_(struct out *o, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++)
        put(o, s[i]);
}

struct spec { bool left, plus, space, alt, zero; int width, prec; };

static void pad(struct out *o, int n, char c) {
    while (n-- > 0)
        put(o, c);
}

static void number(struct out *o, u64 v, bool neg, unsigned base, bool upper, struct spec s) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    if (v || s.prec != 0) /* (%.0d of 0 prints nothing, as C says) */
        do {
            tmp[n++] = digits[v % base];
            v /= base;
        } while (v);
    char sign = neg ? '-' : s.plus ? '+' : s.space ? ' ' : 0;
    const char *prefix = "";
    if (s.alt && base == 16 && n && !(n == 1 && tmp[0] == '0'))
        prefix = upper ? "0X" : "0x";
    else if (s.alt && base == 8 && (n == 0 || tmp[n - 1] != '0'))
        prefix = "0";
    int digits_n = s.prec > n ? s.prec : n;
    int total = digits_n + (sign ? 1 : 0) + (int)strlen(prefix);
    bool zero = s.zero && !s.left && s.prec < 0;
    if (!s.left && !zero)
        pad(o, s.width - total, ' ');
    if (sign)
        put(o, sign);
    puts_(o, prefix, strlen(prefix));
    if (zero)
        pad(o, s.width - total, '0');
    pad(o, digits_n - n, '0');
    while (n)
        put(o, tmp[--n]);
    if (s.left)
        pad(o, s.width - total, ' ');
}

static void string(struct out *o, const char *str, struct spec s) {
    if (!str)
        str = "(null)";
    size_t l = s.prec >= 0 ? strnlen(str, (size_t)s.prec) : strlen(str);
    if (!s.left)
        pad(o, s.width - (int)l, ' ');
    puts_(o, str, l);
    if (s.left)
        pad(o, s.width - (int)l, ' ');
}

static void hex_bytes(struct out *o, const u8 *b, int n, char sep, bool upper) {
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        if (i && sep)
            put(o, sep);
        put(o, d[b[i] >> 4]);
        put(o, d[b[i] & 15]);
    }
}

/* %p and its extensions; returns how many characters of the format after the 'p' it used */
static int pointer(struct out *o, const char *f, const void *p, struct spec s) {
    struct spec hex = {.zero = true, .width = 16, .prec = -1};
    char tmp[64];
    struct out t = {tmp, sizeof tmp, 0};
    int used = 0;
    switch (f[0]) {
    case 'a': { /* %pa: a phys_addr_t's value (%pad: dma_addr_t; %pap: phys) */
        used = 1 + (f[1] == 'd' || f[1] == 'p');
        u64 v = p ? *(const u64 *)p : 0;
        put(&t, '0'), put(&t, 'x');
        number(&t, v, false, 16, false, hex);
        break;
    }
    case 'U': { /* %pUb %pUB %pUl %pUL: a UUID */
        used = 1 + (f[1] == 'b' || f[1] == 'B' || f[1] == 'l' || f[1] == 'L');
        bool upper = f[1] == 'B' || f[1] == 'L', little = f[1] == 'l' || f[1] == 'L';
        static const u8 be[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        static const u8 le[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        const u8 *idx = little ? le : be, *b = p;
        for (int i = 0; b && i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                put(&t, '-');
            hex_bytes(&t, &b[idx[i]], 1, 0, upper);
        }
        break;
    }
    case 'M': /* %pM: a MAC address, colons; %pm: without */
    case 'm':
        used = 1 + (f[1] == 'R' || f[1] == 'F');
        if (p)
            hex_bytes(&t, p, 6, f[0] == 'M' ? (f[1] == 'F' ? '-' : ':') : 0, false);
        break;
    case 'I': /* %pI4 / %pi4: an IPv4 address (%pI6 printed as hex) */
    case 'i':
        used = 1 + (f[1] == '4' || f[1] == '6');
        if (p && f[1] == '4') {
            const u8 *b = p;
            for (int i = 0; i < 4; i++) {
                if (i)
                    put(&t, '.');
                number(&t, b[i], false, 10, false, (struct spec){.prec = -1});
            }
        } else if (p) {
            hex_bytes(&t, p, 16, ':', false);
        }
        break;
    case 'h': { /* %*ph: a small buffer in hex, the width its length */
        used = 1 + (f[1] == 'C' || f[1] == 'D' || f[1] == 'N');
        char sep = f[1] == 'C' ? ':' : f[1] == 'D' ? '-' : f[1] == 'N' ? 0 : ' ';
        int n = s.width > 0 && s.width <= 64 ? s.width : 1;
        if (p) {
            hex_bytes(o, p, n, sep, false);
            return used;
        }
        break;
    }
    default:
        /* %p %px %pK %pS %ps %pB %pF %pf and the others: the address itself (nothing here is kept from whoever
         * reads the log, and nothing names symbols) */
        while (isalnum((unsigned char)f[used]))
            used++;
        if (!p) {
            string(o, "(null)", s);
            return used;
        }
        put(&t, '0'), put(&t, 'x');
        number(&t, (u64)(uintptr_t)p, false, 16, false, hex);
    }
    t.buf[t.len < t.size ? t.len : t.size - 1] = 0;
    s.prec = -1;
    string(o, tmp, s);
    return used;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct out o = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        const char *start = fmt++;
        struct spec s = {.prec = -1};
        for (;; fmt++) {
            if (*fmt == '-')
                s.left = true;
            else if (*fmt == '+')
                s.plus = true;
            else if (*fmt == ' ')
                s.space = true;
            else if (*fmt == '#')
                s.alt = true;
            else if (*fmt == '0')
                s.zero = true;
            else
                break;
        }
        if (*fmt == '*') {
            s.width = va_arg(ap, int);
            if (s.width < 0)
                s.left = true, s.width = -s.width;
            fmt++;
        } else {
            while (isdigit((unsigned char)*fmt))
                s.width = s.width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            s.prec = 0;
            if (*fmt == '*') {
                s.prec = va_arg(ap, int);
                if (s.prec < 0)
                    s.prec = -1;
                fmt++;
            } else {
                while (isdigit((unsigned char)*fmt))
                    s.prec = s.prec * 10 + (*fmt++ - '0');
            }
        }
        int len = 0; /* -2 hh, -1 h, 0 int, 1 long, 2 long long (z t j: long) */
        for (;; fmt++) {
            if (*fmt == 'h')
                len = len <= -1 ? -2 : -1;
            else if (*fmt == 'l')
                len = len >= 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j' || *fmt == 'L' || *fmt == 'q')
                len = 2;
            else
                break;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            s64 v = len >= 1 ? (len == 1 ? va_arg(ap, long) : va_arg(ap, long long)) : va_arg(ap, int);
            if (len == -1)
                v = (short)v;
            else if (len == -2)
                v = (signed char)v;
            number(&o, v < 0 ? -(u64)v : (u64)v, v < 0, 10, false, s);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = len >= 1 ? (len == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned long long)) : va_arg(ap, unsigned);
            if (len == -1)
                v = (unsigned short)v;
            else if (len == -2)
                v = (unsigned char)v;
            s.plus = s.space = false;
            number(&o, v, false, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16, *fmt == 'X', s);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!s.left)
                pad(&o, s.width - 1, ' ');
            put(&o, c);
            if (s.left)
                pad(&o, s.width - 1, ' ');
            break;
        }
        case 's':
            string(&o, va_arg(ap, const char *), s);
            break;
        case 'p':
            fmt += pointer(&o, fmt + 1, va_arg(ap, const void *), s);
            break;
        case 'n': /* (Linux ignores %n: nothing is written through it) */
            (void)va_arg(ap, void *);
            break;
        case '%':
            put(&o, '%');
            break;
        case '\0':
            puts_(&o, start, (size_t)(fmt - start));
            fmt--;
            break;
        default: /* something unknown: shown as it is (it takes no argument) */
            puts_(&o, start, (size_t)(fmt - start + 1));
        }
    }
    if (size)
        buf[o.len < size ? o.len : size - 1] = 0;
    return (int)(o.len > INT_MAX ? INT_MAX : o.len);
}
