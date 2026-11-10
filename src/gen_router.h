#ifndef SG_GEN_ROUTER_H
#define SG_GEN_ROUTER_H

#include "gen_rust.h" /* GenSet */
#include "system.h"

/* The C system router, generated at the system root: the root CMakeLists.txt
 * and router/ (queues, routed HALs, step functions, tests). */
void gen_router(const SgSystem *s, GenSet *out);

#endif
