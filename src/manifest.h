#ifndef SG_MANIFEST_H
#define SG_MANIFEST_H

#include "gen_rust.h"
#include "model.h"

#define MANIFEST_PATH ".fsm/manifest.json"

typedef struct {
    char *model_hash;
    StrList paths;
    StrList hashes;
} Manifest;

int manifest_write(const Model *m, const GenSet *g);
/* Returns 0 if the manifest exists and parsed. */
int manifest_read(Manifest *mf);
const char *manifest_hash_of(const Manifest *mf, const char *path);

#endif
