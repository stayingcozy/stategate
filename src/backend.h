#ifndef SG_BACKEND_H
#define SG_BACKEND_H

#include "gen_rust.h"
#include "model.h"

/* One code generator per language. A project picks one language (the
 * model's `language`) and only that backend runs: gen, the language checks
 * of the gate, and the file paths tickets and trace tags point at. */
typedef struct {
    const char *language;
    const char *ext;   /* source extension of node files, e.g. ".rs" */
    void (*gen)(const SgModel *m, GenSet *out);
    /* Gate check 6: no catch-all branch in generated dispatch. */
    void (*check_wildcards)(const GenSet *g, StrList *errors);
    /* Gate check 11: builds and runs the project's tests. */
    void (*run_tests)(StrList *errors, StrList *notes);
    /* Build outputs `stategate init` adds to .gitignore; NULL-terminated. */
    const char *const *gitignore;
} Backend;

/* The backend for the model's language. model_load has already refused a
 * language with no backend, so this never returns NULL for a loaded model. */
const Backend *backend_for(const SgModel *m);
const Backend *backend_named(const char *language);

/* Where a node's hand-written file lives: kind is "actions" or "services". */
void backend_node_path(const Backend *b, const char *kind, const char *id, char *out, size_t n);
/* Where a ticket's logic test for a node lives. */
void backend_logic_test_path(const Backend *b, const char *node, char *out, size_t n);

#endif
