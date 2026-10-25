#ifndef SG_UTIL_H
#define SG_UTIL_H

#include <stddef.h>

/* Growable string. */
typedef struct {
    char *s;
    size_t len, cap;
} SB;

void sb_init(SB *b);
void sb_puts(SB *b, const char *s);
void sb_printf(SB *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Growable list of owned strings. */
typedef struct {
    char **v;
    int n, cap;
} StrList;

void sl_push(StrList *l, const char *s);
void sl_pushf(StrList *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sl_contains(const StrList *l, const char *s);

void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t size);
char *xstrdup(const char *s);

/* Returns a NUL-terminated buffer, or NULL if the file can't be read. */
char *read_file(const char *path, size_t *len);
/* Creates parent directories as needed. Returns 0 on success. */
int write_file(const char *path, const char *data, size_t len);
int file_exists(const char *path);
int mkdirs(const char *dir);

void sha256_hex(const void *data, size_t len, char out[65]);

/* Runs a shell command, returns its stdout (and stderr if merged by the
 * caller). *status gets the exit code. */
char *run_capture(const char *cmd, int *status);

/* POWER_ON -> PowerOn */
char *upper_snake_to_camel(const char *s);
/* posture.Off -> posture_off ; start_nudge -> StartNudge handled by camel() */
char *id_to_snake(const char *s);
/* start_nudge -> StartNudge */
char *snake_to_camel(const char *s);

#endif
