#include "ticket.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gate.h"
#include "ydoc.h"

static const char *const TICKET_KEYS[] = {"id", "title", "step", "status", "base", "approval",
                                          "failures", "nodes", "files", NULL};

static int cmp_ticket(const void *a, const void *b) {
    return strcmp(((const Ticket *)a)->id, ((const Ticket *)b)->id);
}

int tickets_load(Tickets *ts, StrList *errors) {
    memset(ts, 0, sizeof *ts);
    DIR *d = opendir(TICKETS_DIR);
    if (!d) return 0;
    int before = errors->n, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 6 || strcmp(e->d_name + n - 5, ".yaml")) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", TICKETS_DIR, e->d_name);
        YReader r;
        if (y_load(&r, path, errors) != 0) { y_free(&r); continue; }
        yaml_node_t *root = y_root(&r);
        if (!y_is_map(root)) { y_err(&r, root, "ticket must be a mapping"); y_free(&r); continue; }
        if (ts->n == cap) {
            cap = cap ? cap * 2 : 8;
            ts->v = xrealloc(ts->v, sizeof(Ticket) * (size_t)cap);
        }
        Ticket *t = &ts->v[ts->n++];
        memset(t, 0, sizeof *t);
        t->file = xstrdup(path);
        y_check_keys(&r, root, "ticket", TICKET_KEYS);
        t->id = y_str(&r, y_get(&r, root, "id"), "id");
        t->title = y_str(&r, y_get(&r, root, "title"), "title");
        t->step = y_str(&r, y_get(&r, root, "step"), "step");
        t->status = y_str(&r, y_get(&r, root, "status"), "status");
        t->base = y_str(&r, y_get(&r, root, "base"), "base");
        t->approval = y_str(&r, y_get(&r, root, "approval"), "approval");
        t->failures = (int)y_int(&r, y_get(&r, root, "failures"), "failures", 0);
        y_strlist(&r, y_get(&r, root, "nodes"), "nodes", &t->nodes);
        y_strlist(&r, y_get(&r, root, "files"), "files", &t->files);
        if (!t->id) { y_err(&r, root, "ticket is missing 'id'"); t->id = xstrdup("?"); }
        if (!t->title) t->title = xstrdup("");
        if (!t->step) t->step = xstrdup("");
        if (!t->status) t->status = xstrdup("open");
        if (!t->base) t->base = xstrdup("");
        if (!t->approval) t->approval = xstrdup("");
        const char *ok[] = {"open", "active", "done", "locked", "split", NULL};
        int good = 0;
        for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], t->status)) good = 1;
        if (!good) y_err(&r, root, "status must be open, active, done, locked or split");
        y_free(&r);
    }
    closedir(d);
    qsort(ts->v, (size_t)ts->n, sizeof(Ticket), cmp_ticket);
    return errors->n == before ? 0 : -1;
}

static void yaml_list(SB *b, const char *key, const StrList *l) {
    if (!l->n) { sb_printf(b, "%s: []\n", key); return; }
    sb_printf(b, "%s:\n", key);
    for (int i = 0; i < l->n; i++) sb_printf(b, "  - \"%s\"\n", l->v[i]);
}

int ticket_save(const Ticket *t) {
    SB b;
    sb_init(&b);
    sb_printf(&b, "id: %s\ntitle: \"%s\"\nstep: %s\nstatus: %s\nbase: \"%s\"\napproval: \"%s\"\nfailures: %d\n",
              t->id, t->title, *t->step ? t->step : "\"\"", t->status, t->base, t->approval ? t->approval : "",
              t->failures);
    yaml_list(&b, "nodes", &t->nodes);
    yaml_list(&b, "files", &t->files);
    return write_file(t->file, b.s, b.len);
}

void ticket_approval_hash(const Ticket *t, char out[65]) {
    SB b;
    sb_init(&b);
    sb_printf(&b, "id %s\nstep %s\nbase %s\n", t->id, t->step, t->base);
    for (int i = 0; i < t->nodes.n; i++) sb_printf(&b, "node %s\n", t->nodes.v[i]);
    for (int i = 0; i < t->files.n; i++) sb_printf(&b, "file %s\n", t->files.v[i]);
    sha256_hex(b.s, b.len, out);
    free(b.s);
}

Ticket *ticket_find(Tickets *ts, const char *id) {
    for (int i = 0; i < ts->n; i++)
        if (!strcmp(ts->v[i].id, id)) return &ts->v[i];
    return NULL;
}

Ticket *ticket_current(Tickets *ts) {
    for (int i = 0; i < ts->n; i++)
        if (!strcmp(ts->v[i].status, "active") || !strcmp(ts->v[i].status, "locked")) return &ts->v[i];
    return NULL;
}

/* Only a person at a terminal can approve a split or a sign-off. This keeps
 * an agent from approving its own work by accident; it is not a security
 * boundary against an agent that sets out to fake a terminal. */
int confirm_by_typing(const char *phrase) {
    fflush(stdout);
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "refused: this needs the architect at an interactive terminal\n");
        return 0;
    }
    printf("Type '%s' to confirm: ", phrase);
    fflush(stdout);
    char line[256];
    if (!fgets(line, sizeof line, stdin)) return 0;
    line[strcspn(line, "\n")] = 0;
    return strcmp(line, phrase) == 0;
}

static int ticket_usage(void) {
    fprintf(stderr,
            "usage: stategate ticket list\n"
            "       stategate ticket new <id> --title \"...\" --step <step> --node <id>... --file <glob>...\n"
            "       stategate ticket check [<id>]\n"
            "       stategate ticket start <id>      (architect only: approves the ticket)\n"
            "       stategate ticket done <id>\n"
            "       stategate ticket split <id> \"<sub-step title>\" \"<sub-step title>\" ...\n");
    return 2;
}

static void print_ticket(const Ticket *t) {
    printf("%s  \"%s\"\n  step:  %s\n  nodes:", t->id, t->title, t->step);
    for (int i = 0; i < t->nodes.n; i++) printf(" %s", t->nodes.v[i]);
    printf("\n  files the agent may change:\n");
    for (int i = 0; i < t->files.n; i++) printf("    %s\n", t->files.v[i]);
}

static int need_model(const char *model_path) {
    if (model_path) return 1;
    fprintf(stderr, "this needs the model: run it in the project directory or pass --model\n");
    return 0;
}

static int print_lint(const char *model_path, const Ticket *t) {
    StrList e = {0};
    ticket_lint(model_path, t, &e);
    for (int i = 0; i < e.n; i++) fprintf(stderr, "  %s\n", e.v[i]);
    return e.n;
}

/* ticket new <id> --title T --step S --node N... --file F...
 * The ticket writer's way in: the ticket is checked against the model and
 * written as open, waiting for the architect. */
static int ticket_new(int argc, char **argv, Tickets *ts, const char *model_path) {
    if (!need_model(model_path)) return 2;
    Ticket t;
    memset(&t, 0, sizeof t);
    t.id = argv[1];
    for (const char *c = t.id; *c; c++)
        if (!(isalnum((unsigned char)*c) || *c == '.' || *c == '-' || *c == '_')) {
            fprintf(stderr, "ticket id '%s' may use only letters, digits, '.', '-' and '_'\n", t.id);
            return 2;
        }
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a); return 2; }
        const char *v = argv[++i];
        if (!strcmp(a, "--title")) t.title = (char *)v;
        else if (!strcmp(a, "--step")) t.step = (char *)v;
        else if (!strcmp(a, "--node")) sl_push(&t.nodes, v);
        else if (!strcmp(a, "--file")) sl_push(&t.files, v);
        else { fprintf(stderr, "unknown option %s\n", a); return ticket_usage(); }
    }
    if (t.title && strpbrk(t.title, "\"\\\n")) { fprintf(stderr, "the title may not contain quotes, backslashes or newlines\n"); return 2; }
    if (ticket_find(ts, t.id)) { fprintf(stderr, "ticket %s already exists\n", t.id); return 1; }
    if (!t.title) t.title = "";
    if (!t.step) t.step = "";
    t.status = "open";
    t.base = "";
    t.approval = "";
    char file[512];
    snprintf(file, sizeof file, "%s/%s.yaml", TICKETS_DIR, t.id);
    t.file = file;
    if (print_lint(model_path, &t)) { fprintf(stderr, "ticket %s not written\n", t.id); return 1; }
    mkdirs(TICKETS_DIR);
    if (ticket_save(&t) != 0) { fprintf(stderr, "cannot write %s\n", file); return 1; }
    printf("wrote %s (open). The architect approves it with: stategate ticket start %s\n", file, t.id);
    return 0;
}

int cmd_ticket(int argc, char **argv, const char *model_path) {
    if (argc < 1) return ticket_usage();
    StrList errors = {0};
    Tickets ts;
    tickets_load(&ts, &errors);
    for (int i = 0; i < errors.n; i++) fprintf(stderr, "%s\n", errors.v[i]);
    if (errors.n) return 1;

    const char *sub = argv[0];
    if (!strcmp(sub, "list")) {
        if (!ts.n) printf("no tickets in %s\n", TICKETS_DIR);
        for (int i = 0; i < ts.n; i++)
            printf("%-10s %-7s fails %d/%d  %s\n", ts.v[i].id, ts.v[i].status, ts.v[i].failures, MAX_FAILURES,
                   ts.v[i].title);
        return 0;
    }
    if (!strcmp(sub, "check")) {
        if (!need_model(model_path)) return 2;
        int bad = 0, n = 0;
        for (int i = 0; i < ts.n; i++) {
            Ticket *c = &ts.v[i];
            if (argc >= 2 ? strcmp(c->id, argv[1]) != 0 : strcmp(c->status, "open") != 0) continue;
            n++;
            if (print_lint(model_path, c)) { printf("%-10s problems above\n", c->id); bad = 1; }
            else printf("%-10s ok\n", c->id);
        }
        if (argc >= 2 && !n) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }
        if (!n) printf("no open tickets\n");
        return bad;
    }
    if (argc < 2) return ticket_usage();
    if (!strcmp(sub, "new")) return ticket_new(argc, argv, &ts, model_path);
    Ticket *t = ticket_find(&ts, argv[1]);
    if (!t) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }

    if (!strcmp(sub, "start")) {
        Ticket *cur = ticket_current(&ts);
        if (cur && cur != t) { fprintf(stderr, "ticket %s is %s; finish or split it first\n", cur->id, cur->status); return 1; }
        if (strcmp(t->status, "open")) { fprintf(stderr, "ticket %s is %s, not open\n", t->id, t->status); return 1; }
        if (!need_model(model_path)) return 2;
        if (print_lint(model_path, t)) { fprintf(stderr, "ticket %s cannot be started; fix it first\n", t->id); return 1; }
        printf("Approve this ticket? An agent will work on it, limited to these files.\n\n");
        print_ticket(t);
        printf("\n");
        if (!confirm_by_typing("approve")) { fprintf(stderr, "not approved; %s stays open\n", t->id); return 1; }
        int st;
        char *head = run_capture("git rev-parse HEAD 2>/dev/null", &st);
        head[strcspn(head, "\n")] = 0;
        if (st != 0 || !*head) { fprintf(stderr, "ticket start needs a git repository with at least one commit\n"); return 1; }
        t->base = head;
        t->status = xstrdup("active");
        t->failures = 0;
        char hash[65];
        ticket_approval_hash(t, hash);
        t->approval = xstrdup(hash);
        ticket_save(t);
        printf("%s approved and active from %.12s\n", t->id, t->base);
        return 0;
    }
    if (!strcmp(sub, "done")) {
        if (strcmp(t->status, "active")) { fprintf(stderr, "ticket %s is %s, not active\n", t->id, t->status); return 1; }
        char *gate = read_file(".fsm/gate.json", NULL);
        char want[256];
        snprintf(want, sizeof want, "\"ticket\": \"%s\"", t->id);
        if (!gate || !strstr(gate, "\"ok\": true") || !strstr(gate, want)) {
            fprintf(stderr, "refused: the last gate run for %s was not green. Run stategate gate.\n", t->id);
            return 1;
        }
        t->status = xstrdup("done");
        ticket_save(t);
        printf("%s done\n", t->id);
        return 0;
    }
    if (!strcmp(sub, "split")) {
        if (argc < 4) { fprintf(stderr, "a split needs at least two sub-step titles\n"); return 2; }
        if (!strcmp(t->status, "done") || !strcmp(t->status, "split")) {
            fprintf(stderr, "ticket %s is %s\n", t->id, t->status);
            return 1;
        }
        printf("Split %s \"%s\" into:\n", t->id, t->title);
        for (int i = 2; i < argc; i++) printf("  %s.%d  %s\n", t->id, i - 1, argv[i]);
        if (!confirm_by_typing("split")) { fprintf(stderr, "not split\n"); return 1; }
        for (int i = 2; i < argc; i++) {
            Ticket s = *t;
            char id[128], file[512];
            snprintf(id, sizeof id, "%s.%d", t->id, i - 1);
            snprintf(file, sizeof file, "%s/%s.yaml", TICKETS_DIR, id);
            s.id = id;
            s.file = file;
            s.title = argv[i];
            s.status = "open";
            s.base = "";
            s.approval = "";
            s.failures = 0;
            ticket_save(&s);
        }
        t->status = xstrdup("split");
        ticket_save(t);
        printf("split done; start the first sub-ticket with: stategate ticket start %s.1\n", t->id);
        return 0;
    }
    return ticket_usage();
}
