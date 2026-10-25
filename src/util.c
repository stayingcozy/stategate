#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

void *xrealloc(void *p, size_t size) {
    p = realloc(p, size ? size : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = xcalloc(n, 1);
    memcpy(d, s, n);
    return d;
}

void sb_init(SB *b) {
    b->cap = 256;
    b->len = 0;
    b->s = xcalloc(b->cap, 1);
}

static void sb_grow(SB *b, size_t extra) {
    if (!b->s) sb_init(b);
    if (b->len + extra + 1 <= b->cap) return;
    while (b->len + extra + 1 > b->cap) b->cap *= 2;
    b->s = xrealloc(b->s, b->cap);
}

void sb_puts(SB *b, const char *s) {
    size_t n = strlen(s);
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

void sb_printf(SB *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    sb_grow(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void sl_push(StrList *l, const char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xrealloc(l->v, sizeof(char *) * (size_t)l->cap);
    }
    l->v[l->n++] = xstrdup(s);
}

void sl_pushf(StrList *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *buf = xcalloc((size_t)n + 1, 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sl_push(l, buf);
    free(buf);
}

int sl_contains(const StrList *l, const char *s) {
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0) return 1;
    return 0;
}

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    SB b;
    sb_init(&b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        sb_grow(&b, n);
        memcpy(b.s + b.len, buf, n);
        b.len += n;
        b.s[b.len] = 0;
    }
    fclose(f);
    if (len) *len = b.len;
    return b.s;
}

int mkdirs(const char *dir) {
    char *p = xstrdup(dir);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/') {
            *c = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST) { free(p); return -1; }
            *c = '/';
        }
    }
    int r = (mkdir(p, 0755) != 0 && errno != EEXIST) ? -1 : 0;
    free(p);
    return r;
}

int write_file(const char *path, const char *data, size_t len) {
    char *dir = xstrdup(path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (mkdirs(dir) != 0) { free(dir); return -1; }
    }
    free(dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

char *run_capture(const char *cmd, int *status) {
    FILE *p = popen(cmd, "r");
    if (!p) { if (status) *status = -1; return xstrdup(""); }
    SB b;
    sb_init(&b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf - 1, p)) > 0) {
        buf[n] = 0;
        sb_puts(&b, buf);
    }
    int st = pclose(p);
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return b.s;
}

char *upper_snake_to_camel(const char *s) {
    char *out = xcalloc(strlen(s) + 1, 1);
    int up = 1, j = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '_') { up = 1; continue; }
        out[j++] = up ? (char)toupper((unsigned char)*c) : (char)tolower((unsigned char)*c);
        up = 0;
    }
    return out;
}

char *snake_to_camel(const char *s) {
    char *out = xcalloc(strlen(s) + 1, 1);
    int up = 1, j = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '_') { up = 1; continue; }
        out[j++] = up ? (char)toupper((unsigned char)*c) : *c;
        up = 0;
    }
    return out;
}

char *id_to_snake(const char *s) {
    /* posture.SlouchGrace -> posture_slouch_grace */
    char *out = xcalloc(strlen(s) * 2 + 1, 1);
    int j = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '.' || *c == '-') { out[j++] = '_'; continue; }
        if (isupper((unsigned char)*c)) {
            if (j > 0 && out[j - 1] != '_') out[j++] = '_';
            out[j++] = (char)tolower((unsigned char)*c);
        } else {
            out[j++] = *c;
        }
    }
    return out;
}

/* ---- SHA-256 (FIPS 180-4) ---- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_hex(const void *data, size_t len, char out[65]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, p + i);
    uint8_t tail[128] = {0};
    size_t rem = len - i;
    memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha256_block(h, tail);
    if (tl == 128) sha256_block(h, tail + 64);
    for (int k = 0; k < 8; k++) sprintf(out + k * 8, "%08x", h[k]);
    out[64] = 0;
}
