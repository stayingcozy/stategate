#include "ydoc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int y_load(YReader *r, const char *path, StrList *errors) {
    memset(r, 0, sizeof *r);
    r->path = path;
    r->errors = errors;
    FILE *f = fopen(path, "rb");
    if (!f) {
        sl_pushf(errors, "%s: cannot open file", path);
        return -1;
    }
    yaml_parser_t p;
    yaml_parser_initialize(&p);
    yaml_parser_set_input_file(&p, f);
    int ok = yaml_parser_load(&p, &r->doc);
    if (!ok) {
        sl_pushf(errors, "%s:%zu: YAML syntax error: %s", path, p.problem_mark.line + 1,
                 p.problem ? p.problem : "unknown");
    }
    yaml_parser_delete(&p);
    fclose(f);
    if (!ok) return -1;
    r->loaded = 1;
    if (!yaml_document_get_root_node(&r->doc)) {
        sl_pushf(errors, "%s: file is empty", path);
        return -1;
    }
    return 0;
}

void y_free(YReader *r) {
    if (r->loaded) yaml_document_delete(&r->doc);
    r->loaded = 0;
}

yaml_node_t *y_root(YReader *r) { return yaml_document_get_root_node(&r->doc); }
yaml_node_t *y_node(YReader *r, int index) { return yaml_document_get_node(&r->doc, index); }

int y_line(yaml_node_t *n) { return n ? (int)n->start_mark.line + 1 : 0; }

void y_err(YReader *r, yaml_node_t *at, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (at)
        sl_pushf(r->errors, "%s:%d: %s", r->path, y_line(at), msg);
    else
        sl_pushf(r->errors, "%s: %s", r->path, msg);
}

int y_is_map(yaml_node_t *n) { return n && n->type == YAML_MAPPING_NODE; }
int y_is_seq(yaml_node_t *n) { return n && n->type == YAML_SEQUENCE_NODE; }
int y_is_scalar(yaml_node_t *n) { return n && n->type == YAML_SCALAR_NODE; }

static const char *scalar_text(yaml_node_t *n) { return (const char *)n->data.scalar.value; }

yaml_node_t *y_get(YReader *r, yaml_node_t *map, const char *key) {
    if (!y_is_map(map)) return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = y_node(r, p->key);
        if (y_is_scalar(k) && strcmp(scalar_text(k), key) == 0) return y_node(r, p->value);
    }
    return NULL;
}

void y_check_keys(YReader *r, yaml_node_t *map, const char *where, const char *const *allowed) {
    if (!y_is_map(map)) return;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start; p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = y_node(r, p->key);
        if (!y_is_scalar(k)) {
            y_err(r, k, "%s: keys must be plain strings", where);
            continue;
        }
        int ok = 0;
        for (const char *const *a = allowed; *a; a++)
            if (strcmp(*a, scalar_text(k)) == 0) ok = 1;
        if (!ok) y_err(r, k, "%s: unknown key '%s'", where, scalar_text(k));
    }
}

char *y_str(YReader *r, yaml_node_t *n, const char *what) {
    if (!n) return NULL;
    if (!y_is_scalar(n)) {
        y_err(r, n, "%s must be a string", what);
        return NULL;
    }
    return xstrdup(scalar_text(n));
}

int y_bool(YReader *r, yaml_node_t *n, const char *what, int def) {
    if (!n) return def;
    if (y_is_scalar(n)) {
        const char *s = scalar_text(n);
        if (!strcmp(s, "true")) return 1;
        if (!strcmp(s, "false")) return 0;
    }
    y_err(r, n, "%s must be true or false", what);
    return def;
}

long y_int(YReader *r, yaml_node_t *n, const char *what, long def) {
    if (!n) return def;
    if (y_is_scalar(n)) {
        char *end;
        long v = strtol(scalar_text(n), &end, 10);
        if (*scalar_text(n) && !*end) return v;
    }
    y_err(r, n, "%s must be an integer", what);
    return def;
}

double y_num(YReader *r, yaml_node_t *n, const char *what, double def) {
    if (!n) return def;
    if (y_is_scalar(n)) {
        char *end;
        double v = strtod(scalar_text(n), &end);
        if (*scalar_text(n) && !*end) return v;
    }
    y_err(r, n, "%s must be a number", what);
    return def;
}

void y_strlist(YReader *r, yaml_node_t *n, const char *what, StrList *out) {
    if (!n) return;
    if (!y_is_seq(n)) {
        y_err(r, n, "%s must be a list", what);
        return;
    }
    for (yaml_node_item_t *i = n->data.sequence.items.start; i < n->data.sequence.items.top; i++) {
        yaml_node_t *item = y_node(r, *i);
        if (!y_is_scalar(item)) {
            y_err(r, item, "%s items must be strings", what);
            continue;
        }
        sl_push(out, scalar_text(item));
    }
}
