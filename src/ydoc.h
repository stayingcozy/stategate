#ifndef SG_YDOC_H
#define SG_YDOC_H

#include <yaml.h>

#include "util.h"

/* Thin helpers over libyaml's document API. Errors are appended to the
 * reader's error list as "path:line: message" instead of aborting, so a
 * single run reports every schema problem. */
typedef struct {
    yaml_document_t doc;
    const char *path;
    StrList *errors;
    int loaded;
} YReader;

int y_load(YReader *r, const char *path, StrList *errors);
void y_free(YReader *r);
yaml_node_t *y_root(YReader *r);
yaml_node_t *y_node(YReader *r, int index);

void y_err(YReader *r, yaml_node_t *at, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

int y_is_map(yaml_node_t *n);
int y_is_seq(yaml_node_t *n);
int y_is_scalar(yaml_node_t *n);

yaml_node_t *y_get(YReader *r, yaml_node_t *map, const char *key);
/* Reports any key not in the NULL-terminated allowed list. */
void y_check_keys(YReader *r, yaml_node_t *map, const char *where, const char *const *allowed);

/* Each returns a default (NULL / 0 / def) and records an error when the
 * node is the wrong type. A NULL node yields the default silently. */
char *y_str(YReader *r, yaml_node_t *n, const char *what);
int y_bool(YReader *r, yaml_node_t *n, const char *what, int def);
long y_int(YReader *r, yaml_node_t *n, const char *what, long def);
double y_num(YReader *r, yaml_node_t *n, const char *what, double def);
void y_strlist(YReader *r, yaml_node_t *n, const char *what, StrList *out);
int y_line(yaml_node_t *n);

#endif
