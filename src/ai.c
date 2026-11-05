#include "ai.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "util.h"

/* Your AI over MCP: Claude Code, run with only the stategate MCP server.
 * The instructions are short because the tools carry the task: get_task
 * returns the whole brief, and write_file refuses files outside the ticket. */

static const char *const FILL_PROMPT =
    "You are the ticket writer for this stategate project. Use only the stategate MCP tools. "
    "Call list_drafts, then call fill_ticket once for each draft with a short title and a goal: "
    "what the node must do in this step, testable, in a few sentences. Read the model or code "
    "with read_file when you need it. Do not change any other file. When every draft is filled, "
    "say which tickets you filled.";

static const char *const WORK_PROMPT =
    "You are the implementer for this stategate project. Use only the stategate MCP tools. "
    "Call get_task for the active ticket, change only the files it allows with write_file "
    "(whole files, plus a CHANGELOG.md entry naming the nodes), then call run_gate. "
    "Where the brief says to run a stategate command, call the matching tool. "
    "If the gate is red, call get_task again and fix what it reports. Stop when the gate is "
    "green or the ticket locks, and report the gate result exactly as run_gate gave it.";

static void self_path(char out[PATH_MAX], const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", out, PATH_MAX - 1);
    if (n > 0) { out[n] = 0; return; }
    if (!realpath(argv0, out)) snprintf(out, PATH_MAX, "%s", argv0);
}

/* {"mcpServers":{"stategate":{"command":<this binary>,"args":["-C",<project>,"mcp"]}}} */
static char *mcp_config_json(const char *argv0, int pretty) {
    char self[PATH_MAX], cwd[PATH_MAX];
    self_path(self, argv0);
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    cJSON *root = cJSON_CreateObject();
    cJSON *servers = cJSON_AddObjectToObject(root, "mcpServers");
    cJSON *sg = cJSON_AddObjectToObject(servers, "stategate");
    cJSON_AddStringToObject(sg, "command", self);
    cJSON *args = cJSON_AddArrayToObject(sg, "args");
    cJSON_AddItemToArray(args, cJSON_CreateString("-C"));
    cJSON_AddItemToArray(args, cJSON_CreateString(cwd));
    cJSON_AddItemToArray(args, cJSON_CreateString("mcp"));
    char *s = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

int cmd_mcp_config(const char *argv0) {
    char *s = mcp_config_json(argv0, 1);
    printf("%s\n", s);
    free(s);
    return 0;
}

static int have_claude(void) {
    int st;
    char *out = run_capture("command -v claude 2>/dev/null", &st);
    free(out);
    if (st == 0) return 1;
    fprintf(stderr, "claude is not on PATH: install Claude Code (https://claude.com/claude-code), "
                    "or use stategate mcp config with another MCP client\n");
    return 0;
}

/* Prints the command, shell-quoted, then becomes it. */
static int exec_shown(char *const argv[]) {
    printf("$");
    for (int i = 0; argv[i]; i++) {
        const char *a = argv[i];
        int plain = *a != 0;
        for (const char *c = a; *c; c++)
            if (!(isalnum((unsigned char)*c) || strchr("-_./=:@", *c))) plain = 0;
        if (plain) printf(" %s", a);
        else if (strlen(a) > 60) printf(" '%.40s...'", a);
        else printf(" '%s'", a);
    }
    printf("\n");
    fflush(stdout);
    execvp(argv[0], argv);
    perror(argv[0]);
    return 127;
}

static int ai_run(const char *prompt, const char *argv0) {
    if (!have_claude()) return 2;
    char *cfg = mcp_config_json(argv0, 0);
    char *argv[] = {"claude", "-p", (char *)prompt,
                    "--mcp-config", cfg, "--strict-mcp-config",
                    "--tools", "",                    /* no built-in tools: no shell, no editor */
                    "--allowedTools", "mcp__stategate", /* every stategate tool, nothing else */
                    "--permission-mode", "dontAsk", NULL};
    return exec_shown(argv);
}

static int ai_usage(void) {
    fprintf(stderr,
            "usage: stategate ai fill      (Claude Code fills in the draft tickets over MCP)\n"
            "       stategate ai work      (Claude Code works the active ticket until the gate is green)\n"
            "       stategate ai connect   (register the stategate MCP server with Claude Code for this project)\n"
            "       stategate ai check     (ask Claude Code whether it can reach the server)\n");
    return 2;
}

int cmd_ai(int argc, char **argv, const char *argv0) {
    if (argc < 1) return ai_usage();
    if (!strcmp(argv[0], "fill")) return ai_run(FILL_PROMPT, argv0);
    if (!strcmp(argv[0], "work")) return ai_run(WORK_PROMPT, argv0);
    if (!strcmp(argv[0], "connect")) {
        if (!have_claude()) return 2;
        char self[PATH_MAX], cwd[PATH_MAX];
        self_path(self, argv0);
        if (!getcwd(cwd, sizeof cwd)) return 1;
        /* Replace an older registration, e.g. one pointing at another build. */
        int st;
        free(run_capture("claude mcp remove -s local stategate >/dev/null 2>&1", &st));
        char *add[] = {"claude", "mcp", "add", "-s", "local", "stategate", "--", self, "-C", cwd, "mcp", NULL};
        return exec_shown(add);
    }
    if (!strcmp(argv[0], "check")) {
        if (!have_claude()) return 2;
        char *get[] = {"claude", "mcp", "get", "stategate", NULL};
        return exec_shown(get);
    }
    return ai_usage();
}
