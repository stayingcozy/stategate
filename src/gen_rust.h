#ifndef SG_GEN_RUST_H
#define SG_GEN_RUST_H

#include "model.h"

typedef struct {
    char *path;     /* relative to the project root */
    char *content;
    int scaffold;   /* 1 = written once for agents to own; never overwritten or hashed */
} GenFile;

typedef struct {
    GenFile *v;
    int n, cap;
} GenSet;

/* Builds every generated and scaffold file for a valid model, in memory. */
void gen_rust(const Model *m, GenSet *out);

#endif
