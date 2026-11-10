#include "backend.h"

#include <stdio.h>
#include <string.h>

#include "gen_rust.h"

static const char *const RUST_IGNORE[] = {"/target", "Cargo.lock", NULL};

static const Backend BACKENDS[] = {
    {"rust", ".rs", gen_rust, rust_check_wildcards, rust_run_tests, RUST_IGNORE},
};

const Backend *backend_named(const char *language) {
    for (size_t i = 0; i < sizeof BACKENDS / sizeof BACKENDS[0]; i++)
        if (language && !strcmp(BACKENDS[i].language, language)) return &BACKENDS[i];
    return NULL;
}

const Backend *backend_for(const SgModel *m) {
    const Backend *b = backend_named(m->language);
    return b ? b : &BACKENDS[0];
}

void backend_node_path(const Backend *b, const char *kind, const char *id, char *out, size_t n) {
    snprintf(out, n, "src/%s/%s%s", kind, id, b->ext);
}

void backend_logic_test_path(const Backend *b, const char *node, char *out, size_t n) {
    snprintf(out, n, "tests/logic_%s%s", node, b->ext);
}
