#pragma once
#include <stddef.h>

/* Minimal HTTP/1.1 client with TLS (mbedTLS) for talking to AI providers.
 * One request per connection ("Connection: close"); handles Content-Length
 * and chunked bodies. Certificates are verified against the bundled Mozilla
 * CA list at /etc/ssl/certs/ca-certificates.crt. */

struct http_response {
    int status;      /* 0 if the request never got a response */
    char *body;      /* NUL-terminated, malloc'd */
    size_t body_len;
    char error[256]; /* human-readable reason when status == 0 */
    char location[4096]; /* a redirect's Location header, if it had one (signed CDN links are long) */
};

/* headers: NULL-terminated list of "Name: value" strings (may be NULL). */
int http_request(const char *method, const char *url, const char *const *headers, const char *body,
                 size_t body_len, int timeout_s, struct http_response *out);

/* As http_request, and `on_body` also receives the body as it arrives
 * (chunked transfer already decoded), for replies that stream (SSE). The
 * whole body is still in `out` at the end. */
typedef void (*http_body_fn)(const char *data, size_t len, void *ctx);
int http_request_stream(const char *method, const char *url, const char *const *headers, const char *body,
                        size_t body_len, int timeout_s, http_body_fn on_body, void *ctx, struct http_response *out);
void http_response_free(struct http_response *r);

/* A socket of an app's own (Facet 2's net.connect): TCP, or TLS with the
 * same trusted certificates. Opening waits on the network (call it off the
 * main thread); afterwards reading never waits. */
#include <stdbool.h>
typedef struct tcp_stream tcp_stream;
tcp_stream *tcp_open(const char *host, int port, bool tls, char *err, size_t errlen);
/* Bytes read (> 0), 0 when nothing has arrived yet, -1 when it closed or failed. */
int tcp_read(tcp_stream *s, void *buf, size_t len);
/* All of it sent, or false. */
bool tcp_write(tcp_stream *s, const void *data, size_t len);
int tcp_fd(const tcp_stream *s);
void tcp_close(tcp_stream *s);
