#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai.h"
#include "gate.h"
#include "mcp.h"
#include "platform.h"
#include "ticket.h"
#include "version.h"


static int usage(void) {
    fprintf(stderr,
            "stategate " STATEGATE_VERSION " - model-first development with a deterministic gate\n\n"
            "usage: stategate [-C dir] [--model file.yaml] <command>\n\n"
            "  status [--json]    where the project is: git, init, model, gen, sign-off, tickets, gate\n"
            "  init               set up .fsm/, AGENTS.md, CHANGELOG.md and the pre-commit hook\n"
            "  validate           run gate checks 1-3 on the model\n"
            "  gen                generate Rust wiring, tests and the manifest; scaffold node files\n"
            "  gate [--release]   run all 11 checks and write .fsm/gate.json\n"
            "  signoff            architect only: approve the current locked items\n"
            "  ticket ...         gen | fill <id> ... | new <id> ... | check [<id>] | list\n"
            "                     start [<id>] | prompt [<id> | --drafts] | run | next | done <id>\n"
            "                     split <id> \"title\" \"title\" ...   (start and split: architect only)\n"
            "  export mermaid     print the machine as a Mermaid state diagram\n"
            "  mcp                serve the AI steps as MCP tools over stdin/stdout\n"
            "  mcp config         the MCP server entry for this project, as JSON for any MCP client\n"
            "  ai ...             fill | work | connect | check: Claude Code over the stategate MCP server\n");
    return 2;
}

int main(int argc, char **argv) {
    sg_platform_init();
    const char *model = NULL;
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-C") && i + 1 < argc) {
            if (chdir(argv[++i]) != 0) { perror(argv[i]); return 2; }
        } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            model = argv[++i];
        } else if (!strcmp(argv[i], "--version")) {
            printf("stategate " STATEGATE_VERSION "\n");
            return 0;
        } else {
            break;
        }
    }
    if (i >= argc) return usage();
    const char *cmd = argv[i++];

    StrList errors = {0};
    char *found = NULL;
    if (!strcmp(cmd, "mcp") && i < argc && !strcmp(argv[i], "config")) return cmd_mcp_config(argv[0]);
    if (!strcmp(cmd, "ai")) return cmd_ai(argc - i, argv + i, argv[0]);
    if (!strcmp(cmd, "ticket")) {
        /* list, done and split work without a model; new, check and start need it. */
        if (!model) model = model_find(".", &errors);
        return cmd_ticket(argc - i, argv + i, model);
    }
    if (!strcmp(cmd, "status")) {
        if (!model) model = model_find(".", &errors);
        return cmd_status(model, i < argc && !strcmp(argv[i], "--json"));
    }
    if (!strcmp(cmd, "init") && !model) {
        /* init comes before the model exists: name the one to write. */
        model = model_find(".", &errors);
        if (!model) {
            static char name[512];
            char cwd[400];
            const char *base = sg_getcwd(cwd, sizeof cwd) ? strrchr(cwd, '/') : NULL;
            snprintf(name, sizeof name, "%s.yaml", base && base[1] ? base + 1 : "model");
            model = name;
        }
        return cmd_init(model);
    }
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
    if (!strcmp(cmd, "mcp")) return cmd_mcp(model, argv[0]);
    if (!strcmp(cmd, "export") && i < argc && !strcmp(argv[i], "mermaid")) return cmd_export_mermaid(model);
    return usage();
}
