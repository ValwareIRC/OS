#define _GNU_SOURCE
/* Looking things up on the web, for the companion ([[search:...]] and
 * [[read:https://...]]). Runs in a child process (it waits on the network):
 * a search asks DuckDuckGo's plain-HTML page (no account, no key), and
 * Wikipedia's API when that gives nothing; reading a page fetches it
 * (following a few redirects) and keeps its words, not its markup. */
#include "web.h"
#include "../lib/http.h"
#include "../lib/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define UA "User-Agent: Mozilla/5.0 (X11; Linux x86_64) OperatingSystem/0.9"

static void urlencode(sbuf *b, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p))
            sb_addn(b, (const char *)p, 1);
        else if (*p == ' ')
            sb_add(b, "+");
        else
            sb_printf(b, "%%%02X", *p);
    }
}

static int hexv(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }

static void urldecode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
            s += 2;
        } else
            *o++ = *s == '+' ? ' ' : *s;
    }
    *o = 0;
}

/* Markup to plain words: tags gone (scripts and styles with their insides),
 * the common entities decoded, white space run together. */
static void html_text(const char *h, size_t len, sbuf *out, size_t max) {
    bool space = true;
    for (size_t i = 0; i < len && out->len < max; i++) {
        if (h[i] == '<') {
            if (!strncasecmp(h + i, "<script", 7) || !strncasecmp(h + i, "<style", 6) || !strncasecmp(h + i, "<noscript", 9)) {
                const char *end = strncasecmp(h + i, "<script", 7) == 0 ? "</script" : !strncasecmp(h + i, "<style", 6) ? "</style" : "</noscript";
                const char *e = strcasestr(h + i, end);
                i = e ? (size_t)(e - h) : len;
            }
            bool block = !strncasecmp(h + i, "<p", 2) || !strncasecmp(h + i, "<br", 3) || !strncasecmp(h + i, "<li", 3) ||
                         !strncasecmp(h + i, "<h", 2) || !strncasecmp(h + i, "<div", 4) || !strncasecmp(h + i, "<tr", 3);
            const char *gt = memchr(h + i, '>', len - i);
            i = gt ? (size_t)(gt - h) : len;
            if (block && !space) {
                sb_add(out, "\n");
                space = true;
            }
            continue;
        }
        if (h[i] == '&') {
            static const struct {
                const char *e, *c;
            } ents[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"},
                        {"&nbsp;", " "}, {"&apos;", "'"}};
            bool done = false;
            for (size_t k = 0; k < sizeof ents / sizeof ents[0] && !done; k++)
                if (!strncmp(h + i, ents[k].e, strlen(ents[k].e))) {
                    sb_add(out, ents[k].c);
                    i += strlen(ents[k].e) - 1;
                    space = false;
                    done = true;
                }
            if (done)
                continue;
        }
        if (isspace((unsigned char)h[i])) {
            if (!space)
                sb_add(out, " ");
            space = true;
            continue;
        }
        sb_addn(out, h + i, 1);
        space = false;
    }
}

/* A GET that follows up to four redirects. */
static void get(const char *url, struct http_response *r) {
    char cur[1024];
    snprintf(cur, sizeof cur, "%s", url);
    const char *headers[] = {UA, "Accept: text/html,application/json;q=0.9,*/*;q=0.5", NULL};
    for (int hop = 0;; hop++) {
        http_request("GET", cur, headers, NULL, 0, 20, r);
        if (hop >= 4 || r->status < 300 || r->status >= 400 || !r->location[0])
            return;
        char next[1024];
        if (!strncmp(r->location, "http", 4))
            snprintf(next, sizeof next, "%s", r->location);
        else { /* a path on the same site */
            const char *host_end = strchr(strstr(cur, "://") ? strstr(cur, "://") + 3 : cur, '/');
            int keep = host_end ? (int)(host_end - cur) : (int)strlen(cur);
            snprintf(next, sizeof next, "%.*s%s%s", keep, cur, r->location[0] == '/' ? "" : "/", r->location);
        }
        snprintf(cur, sizeof cur, "%s", next);
        http_response_free(r);
    }
}

static int search_ddg(const char *q, sbuf *out) {
    sbuf url;
    sb_init(&url);
    sb_add(&url, "https://html.duckduckgo.com/html/?q=");
    urlencode(&url, q);
    struct http_response r;
    get(url.p, &r);
    sb_free(&url);
    int n = 0;
    const char *p = r.status == 200 ? r.body : NULL;
    while (p && n < 6 && (p = strstr(p, "class=\"result__a\""))) {
        const char *href = strstr(p, "href=\""), *close = strchr(p, '>');
        if (!href || !close)
            break;
        href += 6;
        const char *hend = strchr(href, '"');
        const char *tend = strstr(close, "</a>");
        if (!hend || !tend)
            break;
        char link[1024];
        snprintf(link, sizeof link, "%.*s", (int)(hend - href), href);
        /* DuckDuckGo's own redirect: the real address is in uddg= */
        char *u = strstr(link, "uddg=");
        if (u) {
            memmove(link, u + 5, strlen(u + 5) + 1);
            char *amp = strchr(link, '&');
            if (amp)
                *amp = 0;
            urldecode(link);
        }
        sbuf title, snip;
        sb_init(&title);
        sb_init(&snip);
        html_text(close + 1, (size_t)(tend - close - 1), &title, 300);
        const char *s = strstr(tend, "class=\"result__snippet\"");
        const char *next = strstr(tend, "class=\"result__a\"");
        if (s && (!next || s < next)) {
            const char *sb = strchr(s, '>'), *se = sb ? strstr(sb, "</a>") : NULL;
            if (sb && se)
                html_text(sb + 1, (size_t)(se - sb - 1), &snip, 400);
        }
        if (strncmp(link, "http", 4) == 0 && !strstr(link, "duckduckgo.com/y.js")) { /* not an ad */
            sb_printf(out, "%d. %s\n   %s\n   %s\n", n + 1, title.p ? title.p : "", link, snip.p ? snip.p : "");
            n++;
        }
        sb_free(&title);
        sb_free(&snip);
        p = tend;
    }
    http_response_free(&r);
    return n;
}

static int search_wikipedia(const char *q, sbuf *out) {
    sbuf url;
    sb_init(&url);
    sb_add(&url, "https://en.wikipedia.org/w/api.php?action=query&list=search&format=json&srlimit=5&srsearch=");
    urlencode(&url, q);
    struct http_response r;
    get(url.p, &r);
    sb_free(&url);
    json *j = r.status == 200 && r.body ? json_parse(r.body, r.body_len) : NULL;
    int n = 0;
    json *list = json_path(j, "query.search");
    for (json *e = list ? list->child : NULL; e && n < 5; e = e->next, n++) {
        const char *title = json_str(json_get(e, "title"), "");
        sbuf snip, link;
        sb_init(&snip);
        sb_init(&link);
        const char *s = json_str(json_get(e, "snippet"), "");
        html_text(s, strlen(s), &snip, 400);
        sb_add(&link, "https://en.wikipedia.org/wiki/");
        for (const char *c = title; *c; c++)
            sb_addn(&link, *c == ' ' ? "_" : c, 1);
        sb_printf(out, "%d. %s (Wikipedia)\n   %s\n   %s\n", n + 1, title, link.p, snip.p ? snip.p : "");
        sb_free(&snip);
        sb_free(&link);
    }
    json_free(j);
    http_response_free(&r);
    return n;
}

static void read_page(const char *url, sbuf *out) {
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) {
        sb_printf(out, "Only web addresses (http or https) can be read: %s\n", url);
        return;
    }
    struct http_response r;
    get(url, &r);
    if (r.status != 200 || !r.body) {
        sb_printf(out, "%s couldn't be read (%s).\n", url, r.status ? "the site said no" : r.error);
        http_response_free(&r);
        return;
    }
    sbuf text;
    sb_init(&text);
    /* the page's own words, not its menus: from <main> or <article> when it marks them */
    const char *from = strcasestr(r.body, "<main");
    if (!from)
        from = strcasestr(r.body, "<article");
    if (!from)
        from = r.body;
    html_text(from, r.body_len - (size_t)(from - r.body), &text, 6000);
    sb_printf(out, "%s:\n%s%s\n", url, text.p ? text.p : "(no words on it)", text.len >= 6000 ? " ..." : "");
    sb_free(&text);
    http_response_free(&r);
}

char *web_lookup(const char *requests) {
    sbuf out;
    sb_init(&out);
    char *copy = strdup(requests);
    for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        const char *what = tab + 1;
        if (!strcmp(line, "search")) {
            sb_printf(&out, "%s[search:%s]\n", out.len ? "\n" : "", what);
            if (!search_ddg(what, &out) && !search_wikipedia(what, &out))
                sb_add(&out, "Nothing came back (the internet may be out of reach).\n");
        } else {
            sb_printf(&out, "%s[read:%s]\n", out.len ? "\n" : "", what);
            read_page(what, &out);
        }
    }
    free(copy);
    return out.p ? out.p : strdup("Nothing came back.");
}
