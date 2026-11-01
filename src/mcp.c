/* `stategate mcp`: a Model Context Protocol server on stdin/stdout, so any
 * MCP client (Claude Code, Cursor, Claude Desktop, ...) can do the AI steps
 * of the flow. Each tool runs the same CLI command a person would, in a
 * child process with no terminal, so the server can never do more than the
 * CLI allows an agent to do. The architect-only commands (signoff, ticket
 * start, ticket split) are not tools: they need a person at a terminal. */
#include "mcp.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cJSON.h"
#include "ticket.h"
#include "util.h"
#include "version.h"

#define PROTOCOL_VERSION "2025-06-18"
#define MAX_READ_BYTES (256 * 1024)

static char self_exe[PATH_MAX];
static const char *g_model;

/* Runs this binary with argv (no shell), stdin from /dev/null, and returns
 * stdout and stderr together. */
static char *run_self(const char *const *args, int *status) {
    int fd[2];
    if (pipe(fd) != 0) { *status = -1; return xstrdup("pipe failed"); }
    pid_t pid = fork();
    if (pid < 0) { *status = -1; return xstrdup("fork failed"); }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]);
        close(fd[1]);
        const char *argv[32];
        int n = 0;
        argv[n++] = self_exe;
        if (g_model) { argv[n++] = "--model"; argv[n++] = g_model; }
        for (int i = 0; args[i] && n < 31; i++) argv[n++] = args[i];
        argv[n] = NULL;
        execvp(self_exe, (char *const *)argv);
        _exit(127);
    }
    close(fd[1]);
    SB b;
    sb_init(&b);
    char buf[4096];
    ssize_t r;
    while ((r = read(fd[0], buf, sizeof buf - 1)) > 0) {
        buf[r] = 0;
        sb_puts(&b, buf);
    }
    close(fd[0]);
    int st;
    waitpid(pid, &st, 0);
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return b.s;
}

/* ---- tool results ---- */

static cJSON *text_result(const char *text, int is_error) {
    cJSON *r = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(r, "isError", is_error);
    return r;
}

static cJSON *cli_result(const char *const *args) {
    int st;
    char *out = run_self(args, &st);
    cJSON *r = text_result(*out ? out : "(no output)", st != 0);
    free(out);
    return r;
}

static const char *arg_str(cJSON *args, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* A path an agent may name: relative, inside the project, no "..". */
static const char *path_problem(const char *p) {
    if (!p || !*p) return "path is empty";
    if (p[0] == '/') return "path must be relative to the project";
    if (!strcmp(p, "..") || !strncmp(p, "../", 3) || strstr(p, "/../") ||
        (strlen(p) >= 3 && !strcmp(p + strlen(p) - 3, "/..")))
        return "path may not contain '..'";
    return NULL;
}

/* The file, once symlinks are resolved, must still be inside the project. */
static int inside_project(const char *path) {
    char root[PATH_MAX], real[PATH_MAX];
    if (!getcwd(root, sizeof root) || !realpath(path, real)) return 0;
    size_t n = strlen(root);
    return !strncmp(real, root, n) && (real[n] == '/' || real[n] == 0);
}

static cJSON *tool_read_file(cJSON *args) {
    const char *path = arg_str(args, "path");
    const char *bad = path_problem(path);
    if (bad) return text_result(bad, 1);
    if (access(path, F_OK) == 0 && !inside_project(path)) return text_result("refused: path resolves outside the project", 1);
    size_t len = 0;
    char *text = read_file(path, &len);
    if (!text) return text_result("no such file", 1);
    if (len > MAX_READ_BYTES) { free(text); return text_result("file is too large to read here", 1); }
    if (memchr(text, 0, len)) { free(text); return text_result("binary file", 1); }
    cJSON *r = text_result(text, 0);
    free(text);
    return r;
}

static int mkdirs_for(const char *path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = 0;
    return mkdirs(dir);
}

/* Writes a file only when the active ticket allows it. The gate would catch
 * anything else later; refusing here saves the agent a failed run. */
static cJSON *tool_write_file(cJSON *args) {
    const char *path = arg_str(args, "path");
    const char *content = arg_str(args, "content");
    const char *bad = path_problem(path);
    if (bad) return text_result(bad, 1);
    if (!content) return text_result("content is missing", 1);
    StrList errors = {0};
    Tickets ts;
    tickets_load(&ts, &errors);
    Ticket *t = ticket_current(&ts);
    if (!t) return text_result("no active ticket: the architect approves one with stategate ticket start", 1);
    if (strcmp(t->status, "active")) {
        char msg[256];
        snprintf(msg, sizeof msg, "ticket %s is %s; propose a split to the architect instead of changing code", t->id, t->status);
        return text_result(msg, 1);
    }
    int allowed = !strcmp(path, "CHANGELOG.md");
    for (int i = 0; i < t->files.n && !allowed; i++)
        if (fnmatch(t->files.v[i], path, FNM_PATHNAME) == 0) allowed = 1;
    if (!allowed || !strncmp(path, ".fsm", 4)) {
        SB b;
        sb_init(&b);
        sb_printf(&b, "refused: ticket %s does not allow %s. Allowed: CHANGELOG.md", t->id, path);
        for (int i = 0; i < t->files.n; i++) sb_printf(&b, ", %s", t->files.v[i]);
        cJSON *r = text_result(b.s, 1);
        free(b.s);
        return r;
    }
    struct stat sb_;
    if (lstat(path, &sb_) == 0 && S_ISLNK(sb_.st_mode)) return text_result("refused: path is a symbolic link", 1);
    if (mkdirs_for(path) != 0) return text_result("cannot create the directory", 1);
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        if (!inside_project(parent)) return text_result("refused: path resolves outside the project", 1);
    }
    if (0 || write_file(path, content, strlen(content)) != 0)
        return text_result("cannot write the file", 1);
    char msg[PATH_MAX + 64];
    snprintf(msg, sizeof msg, "wrote %s (%zu bytes)", path, strlen(content));
    return text_result(msg, 0);
}

static cJSON *tool_fill_ticket(cJSON *args) {
    const char *id = arg_str(args, "id"), *title = arg_str(args, "title"), *goal = arg_str(args, "goal");
    if (!id || !title || !goal) return text_result("id, title and goal are all required", 1);
    const char *a[] = {"ticket", "fill", id, "--title", title, "--goal", goal, NULL};
    return cli_result(a);
}

static cJSON *tool_list_drafts(void) {
    StrList errors = {0};
    Tickets ts;
    tickets_load(&ts, &errors);
    SB b;
    sb_init(&b);
    int n = 0;
    for (int i = 0; i < ts.n; i++) {
        Ticket *t = &ts.v[i];
        if (strcmp(t->status, "draft")) continue;
        n++;
        sb_printf(&b, "%s  step %s\n  nodes:", t->id, t->step);
        for (int k = 0; k < t->nodes.n; k++) sb_printf(&b, " %s", t->nodes.v[k]);
        sb_puts(&b, "\n  files:");
        for (int k = 0; k < t->files.n; k++) sb_printf(&b, " %s", t->files.v[k]);
        sb_printf(&b, "\n  title: %s\n  goal: %s\n\n", *t->title ? t->title : "(empty)", *t->goal ? t->goal : "(empty)");
    }
    if (!n) sb_puts(&b, "No drafts. The architect writes them with stategate ticket gen.\n");
    else sb_puts(&b, "Fill each with fill_ticket. Use read_file on the node's file and the model to understand it.\n");
    for (int i = 0; i < errors.n; i++) sb_printf(&b, "%s\n", errors.v[i]);
    cJSON *r = text_result(b.s, 0);
    free(b.s);
    return r;
}

/* ---- tool table ---- */

typedef struct {
    const char *name, *description;
    const char *props;    /* JSON object of properties */
    const char *required; /* JSON array */
} Tool;

static const Tool TOOLS[] = {
    {"status",
     "Where the project is: every ticket with its status and failure count, and the active ticket. Start here.",
     "{}", "[]"},
    {"list_drafts",
     "Ticket-writer role. Lists draft tickets with their node, step and files, waiting for a title and goal.",
     "{}", "[]"},
    {"fill_ticket",
     "Ticket-writer role. Gives a draft ticket its title and goal; a complete draft becomes open for the architect "
     "to approve. Node, step and files cannot be changed.",
     "{\"id\":{\"type\":\"string\",\"description\":\"Ticket id, e.g. T001\"},"
     "\"title\":{\"type\":\"string\",\"description\":\"Short title\"},"
     "\"goal\":{\"type\":\"string\",\"description\":\"What the node must do for this step, testable, a few sentences\"}}",
     "[\"id\",\"title\",\"goal\"]"},
    {"get_task",
     "Implementer role. The active ticket as one Markdown brief: goal, where the node runs, the files you may "
     "change with their current code, the rules, and the errors from the last failed run.",
     "{}", "[]"},
    {"read_file",
     "Reads a project file (relative path), e.g. the model YAML, generated code or tests.",
     "{\"path\":{\"type\":\"string\"}}", "[\"path\"]"},
    {"write_file",
     "Implementer role. Writes a whole file. Only the active ticket's files and CHANGELOG.md are allowed.",
     "{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\",\"description\":\"The complete new file\"}}",
     "[\"path\",\"content\"]"},
    {"run_gate",
     "Implementer role. Runs all 11 gate checks for the active ticket. Green commits the work, marks the ticket done "
     "and starts the next approved one. Red saves the errors into the ticket: call get_task again and fix them. "
     "Report the result as it is.",
     "{}", "[]"},
};
#define N_TOOLS (int)(sizeof TOOLS / sizeof TOOLS[0])

static cJSON *tools_list(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "tools");
    for (int i = 0; i < N_TOOLS; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", TOOLS[i].name);
        cJSON_AddStringToObject(t, "description", TOOLS[i].description);
        cJSON *schema = cJSON_CreateObject();
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON_AddItemToObject(schema, "properties", cJSON_Parse(TOOLS[i].props));
        cJSON_AddItemToObject(schema, "required", cJSON_Parse(TOOLS[i].required));
        cJSON_AddItemToObject(t, "inputSchema", schema);
        cJSON_AddItemToArray(arr, t);
    }
    return r;
}

static cJSON *tools_call(cJSON *params) {
    const char *name = arg_str(params, "name");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!name) return NULL;
    if (!strcmp(name, "status")) {
        const char *a[] = {"ticket", "list", NULL};
        return cli_result(a);
    }
    if (!strcmp(name, "list_drafts")) return tool_list_drafts();
    if (!strcmp(name, "fill_ticket")) return tool_fill_ticket(args);
    if (!strcmp(name, "get_task")) {
        const char *a[] = {"ticket", "prompt", NULL};
        return cli_result(a);
    }
    if (!strcmp(name, "read_file")) return tool_read_file(args);
    if (!strcmp(name, "write_file")) return tool_write_file(args);
    if (!strcmp(name, "run_gate")) {
        const char *a[] = {"ticket", "run", NULL};
        return cli_result(a);
    }
    return NULL;
}

/* ---- JSON-RPC over newline-delimited stdio ---- */

static void send_json(cJSON *msg) {
    char *s = cJSON_PrintUnformatted(msg);
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
}

static void reply(cJSON *id, cJSON *result) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(m, "result", result);
    send_json(m);
    cJSON_Delete(m);
}

static void reply_error(cJSON *id, int code, const char *message) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(m, "id", cJSON_Duplicate(id, 1));
    else cJSON_AddNullToObject(m, "id");
    cJSON *e = cJSON_AddObjectToObject(m, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", message);
    send_json(m);
    cJSON_Delete(m);
}

static void handle(cJSON *msg) {
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *method = arg_str(msg, "method");
    cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");
    if (!method) {
        if (id) reply_error(id, -32600, "invalid request");
        return; /* a response to us; we send no requests */
    }
    if (!id) return; /* notifications, e.g. notifications/initialized */

    if (!strcmp(method, "initialize")) {
        cJSON *r = cJSON_CreateObject();
        const char *want = arg_str(params, "protocolVersion");
        cJSON_AddStringToObject(r, "protocolVersion", want ? want : PROTOCOL_VERSION);
        cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
        cJSON_AddStringToObject(info, "name", "stategate");
        cJSON_AddStringToObject(info, "version", STATEGATE_VERSION);
        cJSON_AddStringToObject(r, "instructions",
                                "stategate checks AI-written code against a state-machine model. Call status first. "
                                "As ticket writer: list_drafts, then fill_ticket for each. As implementer: get_task, "
                                "write_file for the allowed files, then run_gate; on red, get_task again and fix. "
                                "Only the architect approves tickets, at a terminal.");
        reply(id, r);
    } else if (!strcmp(method, "ping")) {
        reply(id, cJSON_CreateObject());
    } else if (!strcmp(method, "tools/list")) {
        reply(id, tools_list());
    } else if (!strcmp(method, "tools/call")) {
        cJSON *r = tools_call(params);
        if (r) reply(id, r);
        else reply_error(id, -32602, "unknown tool");
    } else {
        reply_error(id, -32601, "method not found");
    }
}

int cmd_mcp(const char *model_path, const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", self_exe, sizeof self_exe - 1);
    if (n > 0) self_exe[n] = 0;
    else snprintf(self_exe, sizeof self_exe, "%s", argv0); /* found via PATH when it has no '/' */
    g_model = model_path;
    fprintf(stderr, "stategate mcp: serving tools for the project in this directory\n");

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (!len) continue;
        cJSON *msg = cJSON_Parse(line);
        if (!msg) { reply_error(NULL, -32700, "parse error"); continue; }
        if (cJSON_IsArray(msg)) reply_error(NULL, -32600, "batches are not supported");
        else handle(msg);
        cJSON_Delete(msg);
    }
    free(line);
    return 0;
}
