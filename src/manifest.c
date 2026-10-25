#include "manifest.h"

#include <stdio.h>
#include <string.h>

int manifest_write(const Model *m, const GenSet *g) {
    SB b;
    sb_init(&b);
    sb_printf(&b, "{\n  \"generator\": \"stategate\",\n  \"model\": \"%s\",\n  \"model_hash\": \"%s\",\n  \"files\": {\n",
              m->path, m->hash);
    int first = 1;
    for (int i = 0; i < g->n; i++) {
        if (g->v[i].scaffold) continue;
        char hex[65];
        sha256_hex(g->v[i].content, strlen(g->v[i].content), hex);
        sb_printf(&b, "%s    \"%s\": \"%s\"", first ? "" : ",\n", g->v[i].path, hex);
        first = 0;
    }
    sb_puts(&b, "\n  }\n}\n");
    return write_file(MANIFEST_PATH, b.s, b.len);
}

/* The manifest is written only by stategate, one entry per line, so a
 * line scanner is enough to read it back. */
int manifest_read(Manifest *mf) {
    memset(mf, 0, sizeof *mf);
    char *text = read_file(MANIFEST_PATH, NULL);
    if (!text) return -1;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[512], val[128];
        if (sscanf(line, " \"%511[^\"]\": \"%127[^\"]\"", key, val) != 2) continue;
        if (!strcmp(key, "model_hash")) mf->model_hash = xstrdup(val);
        else if (!strcmp(key, "generator") || !strcmp(key, "model")) continue;
        else {
            sl_push(&mf->paths, key);
            sl_push(&mf->hashes, val);
        }
    }
    return mf->model_hash ? 0 : -1;
}

const char *manifest_hash_of(const Manifest *mf, const char *path) {
    for (int i = 0; i < mf->paths.n; i++)
        if (!strcmp(mf->paths.v[i], path)) return mf->hashes.v[i];
    return NULL;
}
