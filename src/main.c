#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "gate.h"
#include "ticket.h"

#define VERSION "0.1.0"

static int usage(void) {
    fprintf(stderr,
            "stategate " VERSION " - model-first development with a deterministic gate\n\n"
            "usage: stategate [-C dir] [--model file.yaml] <command>\n\n"
            "  init               set up .fsm/, AGENTS.md, CHANGELOG.md and the pre-commit hook\n"
            "  validate           run gate checks 1-3 on the model\n"
            "  gen                generate Rust wiring, tests and the manifest; scaffold node files\n"
            "  gate [--release]   run all 11 checks and write .fsm/gate.json\n"
            "  signoff            architect only: approve the current locked items\n"
            "  ticket ...         list | start <id> | done <id> | split <id> \"title\" \"title\" ...\n"
            "  export mermaid     print the machine as a Mermaid state diagram\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *model = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) {
            if (chdir(argv[++i]) != 0) { perror(argv[i]); return 2; }
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            model = argv[++i];
        } else if (!strcmp(argv[i], "--version")) {
            printf("stategate " VERSION "\n");
            return 0;
        } else {
            break;
        }
    }
    if (i >= argc) return usage();
    const char *cmd = argv[i++];

    if (!strcmp(cmd, "ticket")) return cmd_ticket(argc - i, argv + i);

    StrList errors = {0};
    char *found = NULL;
    if (!model) {
        found = model_find(".", &errors);
        if (!found) {
            for (int k = 0; k < errors.n; k++) fprintf(stderr, "%s\n", errors.v[k]);
            return 2;
        }
        model = found;
    }

    if (!strcmp(cmd, "init")) return cmd_init(model);
    if (!strcmp(cmd, "validate")) return cmd_validate(model);
    if (!strcmp(cmd, "gen")) return cmd_gen(model);
    if (!strcmp(cmd, "signoff")) return cmd_signoff(model);
    if (!strcmp(cmd, "gate")) return cmd_gate(model, i < argc && !strcmp(argv[i], "--release"));
    if (!strcmp(cmd, "export") && i < argc && !strcmp(argv[i], "mermaid")) return cmd_export_mermaid(model);
    return usage();
}
