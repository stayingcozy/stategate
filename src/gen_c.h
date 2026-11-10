#ifndef SG_GEN_C_H
#define SG_GEN_C_H

#include "gen_rust.h" /* GenSet */
#include "model.h"

/* Builds every generated and scaffold file of a C project, in memory. */
void gen_c(const SgModel *m, GenSet *out);
/* C's gate checks: no `default:` in generated code, no node including the
 * raw HAL and no node sending an event it does not list in `emits`; then
 * CMake builds and ctest runs the Unity tests. */
void c_check_wildcards(const SgModel *m, const GenSet *g, StrList *errors);
void c_run_tests(StrList *errors, StrList *notes);

#endif
