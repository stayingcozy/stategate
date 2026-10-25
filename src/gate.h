#ifndef SG_GATE_H
#define SG_GATE_H

#include "model.h"

int cmd_validate(const char *model_path);
int cmd_gen(const char *model_path);
int cmd_gate(const char *model_path, int release);
int cmd_signoff(const char *model_path);
int cmd_init(const char *model_path);
int cmd_export_mermaid(const char *model_path);

/* Canonical text of every locked item; its hash is what the architect signs. */
char *locked_canonical(const SgModel *m);

#endif
