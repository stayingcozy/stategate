#include "ticket.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gate.h"
#include "ydoc.h"

static const char *const TICKET_KEYS[] = {"id", "title", "goal", "step", "status", "base", "approval",
                                          "failures", "nodes", "files", "errors", NULL};

/* Natural order, so T9 comes before T10 and T2.1 before T2.10: the queue
 * of approved tickets runs in this order. */
static int cmp_ids(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long na = strtoul(a, &ea, 10), nb = strtoul(b, &eb, 10);
            if (na != nb) return na < nb ? -1 : 1;
            a = ea;
            b = eb;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++;
            b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_ticket(const void *a, const void *b) {
    return cmp_ids(((const Ticket *)a)->id, ((const Ticket *)b)->id);
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
        t->goal = y_str(&r, y_get(&r, root, "goal"), "goal");
        t->step = y_str(&r, y_get(&r, root, "step"), "step");
        t->status = y_str(&r, y_get(&r, root, "status"), "status");
        t->base = y_str(&r, y_get(&r, root, "base"), "base");
        t->approval = y_str(&r, y_get(&r, root, "approval"), "approval");
        t->failures = (int)y_int(&r, y_get(&r, root, "failures"), "failures", 0);
        y_strlist(&r, y_get(&r, root, "nodes"), "nodes", &t->nodes);
        y_strlist(&r, y_get(&r, root, "files"), "files", &t->files);
        y_strlist(&r, y_get(&r, root, "errors"), "errors", &t->errors);
        if (!t->id) { y_err(&r, root, "ticket is missing 'id'"); t->id = xstrdup("?"); }
        if (!t->title) t->title = xstrdup("");
        if (!t->goal) t->goal = xstrdup("");
        if (!t->step) t->step = xstrdup("");
        if (!t->status) t->status = xstrdup("open");
        if (!t->base) t->base = xstrdup("");
        if (!t->approval) t->approval = xstrdup("");
        const char *ok[] = {"draft", "open", "approved", "active", "done", "locked", "split", NULL};
        int good = 0;
        for (int i = 0; ok[i]; i++) if (!strcmp(ok[i], t->status)) good = 1;
        if (!good) y_err(&r, root, "status must be draft, open, approved, active, done, locked or split");
        y_free(&r);
    }
    closedir(d);
    qsort(ts->v, (size_t)ts->n, sizeof(Ticket), cmp_ticket);
    return errors->n == before ? 0 : -1;
}

/* YAML double-quoted scalar. */
static void yaml_q(SB *b, const char *v) {
    sb_puts(b, "\"");
    for (const char *c = v ? v : ""; *c; c++) {
        if (*c == '"' || *c == '\\') sb_printf(b, "\\%c", *c);
        else if (*c == '\n') sb_puts(b, "\\n");
        else if (*c == '\t') sb_puts(b, "\\t");
        else if ((unsigned char)*c < 0x20) continue;
        else sb_printf(b, "%c", *c);
    }
    sb_puts(b, "\"");
}

static void yaml_list(SB *b, const char *key, const StrList *l) {
    if (!l->n) { sb_printf(b, "%s: []\n", key); return; }
    sb_printf(b, "%s:\n", key);
    for (int i = 0; i < l->n; i++) {
        sb_puts(b, "  - ");
        yaml_q(b, l->v[i]);
        sb_puts(b, "\n");
    }
}

int ticket_save(const Ticket *t) {
    SB b;
    sb_init(&b);
    sb_printf(&b, "id: %s\ntitle: ", t->id);
    yaml_q(&b, t->title);
    sb_puts(&b, "\ngoal: ");
    yaml_q(&b, t->goal);
    sb_puts(&b, "\nstep: ");
    yaml_q(&b, t->step);
    sb_printf(&b, "\nstatus: %s\nbase: ", t->status);
    yaml_q(&b, t->base);
    sb_puts(&b, "\napproval: ");
    yaml_q(&b, t->approval);
    sb_printf(&b, "\nfailures: %d\n", t->failures);
    yaml_list(&b, "nodes", &t->nodes);
    yaml_list(&b, "files", &t->files);
    yaml_list(&b, "errors", &t->errors);
    return write_file(t->file, b.s, b.len);
}

void ticket_approval_hash(const Ticket *t, char out[65]) {
    SB b;
    sb_init(&b);
    sb_printf(&b, "id %s\ntitle %s\ngoal %s\nstep %s\nbase %s\n", t->id, t->title, t->goal ? t->goal : "", t->step,
              t->base);
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
            "       stategate ticket gen             (one draft per stub node; no AI)\n"
            "       stategate ticket fill <id> --title \"...\" --goal \"...\"   (draft becomes open)\n"
            "       stategate ticket new <id> --title \"...\" --goal \"...\" --step <step> --node <id>... --file <glob>...\n"
            "       stategate ticket check [<id>]\n"
            "       stategate ticket start           (architect only: review every open ticket)\n"
            "       stategate ticket start <id>      (architect only: approve and start one ticket)\n"
            "       stategate ticket next            (start the next approved ticket)\n"
            "       stategate ticket prompt [<id>]   (the active ticket as one block for any AI)\n"
            "       stategate ticket run             (gate; green commits and starts the next ticket)\n"
            "       stategate ticket done <id>\n"
            "       stategate ticket split <id> \"<sub-step title>\" \"<sub-step title>\" ...\n");
    return 2;
}

static void print_ticket(const Ticket *t) {
    printf("%s  \"%s\"\n", t->id, t->title);
    if (t->goal && *t->goal) printf("  goal:  %s\n", t->goal);
    printf("  step:  %s\n  nodes:", t->step);
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
        else if (!strcmp(a, "--goal")) t.goal = (char *)v;
        else if (!strcmp(a, "--step")) t.step = (char *)v;
        else if (!strcmp(a, "--node")) sl_push(&t.nodes, v);
        else if (!strcmp(a, "--file")) sl_push(&t.files, v);
        else { fprintf(stderr, "unknown option %s\n", a); return ticket_usage(); }
    }
    if (ticket_find(ts, t.id)) { fprintf(stderr, "ticket %s already exists\n", t.id); return 1; }
    if (!t.title) t.title = "";
    if (!t.goal) t.goal = "";
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

/* The architect approved t as it stands. Its base is still empty; the
 * approval is re-hashed with the base when the ticket starts. */
static void approve(Ticket *t) {
    char hash[65];
    t->base = xstrdup("");
    ticket_approval_hash(t, hash);
    t->approval = xstrdup(hash);
    t->status = xstrdup("approved");
    ticket_save(t);
}

/* Starts an approved ticket from HEAD. No terminal needed: the architect
 * approved it already, and the stored hash proves the ticket is unchanged
 * since then. Returns 1 when the ticket is now active. */
static int activate(Ticket *t, const char *model_path) {
    char hash[65];
    char *saved_base = t->base;
    t->base = "";
    ticket_approval_hash(t, hash);
    t->base = saved_base;
    if (!t->approval || strcmp(hash, t->approval)) {
        fprintf(stderr, "ticket %s changed after the architect approved it; it needs approving again\n", t->id);
        return 0;
    }
    if (!need_model(model_path)) return 0;
    if (print_lint(model_path, t)) { fprintf(stderr, "ticket %s no longer fits the model; not started\n", t->id); return 0; }
    int st;
    char *dirty = run_capture("git status --porcelain -- . ':(exclude)" TICKETS_DIR "' 2>&1", &st);
    if (st != 0) { fprintf(stderr, "ticket start needs a git repository: %s", dirty); return 0; }
    if (*dirty) {
        fprintf(stderr, "not starting %s: commit the finished work first, then run stategate ticket next\n%s", t->id, dirty);
        return 0;
    }
    char *head = run_capture("git rev-parse HEAD 2>/dev/null", &st);
    head[strcspn(head, "\n")] = 0;
    if (st != 0 || !*head) { fprintf(stderr, "ticket start needs a git repository with at least one commit\n"); return 0; }
    t->base = head;
    t->status = xstrdup("active");
    t->failures = 0;
    ticket_approval_hash(t, hash);
    t->approval = xstrdup(hash);
    ticket_save(t);
    printf("%s active from %.12s: %s\n", t->id, t->base, t->title);
    return 1;
}

/* Starts the first approved ticket in id order, if there is one. */
static int start_next(Tickets *ts, const char *model_path) {
    for (int i = 0; i < ts->n; i++)
        if (!strcmp(ts->v[i].status, "approved")) return activate(&ts->v[i], model_path) ? 0 : 1;
    printf("no approved tickets waiting\n");
    return 0;
}

/* Reads one answer at a terminal: 'a'pprove, 's'kip or 'q'uit. */
static char ask_review(void) {
    for (;;) {
        printf("approve / skip / quit? ");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof line, stdin)) return 'q';
        line[strcspn(line, "\n")] = 0;
        if (!strcmp(line, "approve")) return 'a';
        if (!strcmp(line, "skip") || !strcmp(line, "s")) return 's';
        if (!strcmp(line, "quit") || !strcmp(line, "q")) return 'q';
        printf("type approve, skip or quit\n");
    }
}

/* `ticket start` with no id: the architect reviews every open ticket in
 * turn. Approved ones join the queue; the first starts when nothing is in
 * progress, and `ticket done` starts each next one. */
static int review_open(Tickets *ts, const char *model_path) {
    if (!need_model(model_path)) return 2;
    int open = 0;
    for (int i = 0; i < ts->n; i++) open += !strcmp(ts->v[i].status, "open");
    if (!open) printf("no open tickets to review\n");
    else if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "refused: this needs the architect at an interactive terminal\n");
        return 1;
    }
    int approved = 0, seen = 0;
    for (int i = 0; i < ts->n && open; i++) {
        Ticket *t = &ts->v[i];
        if (strcmp(t->status, "open")) continue;
        printf("\n[%d/%d] ", ++seen, open);
        print_ticket(t);
        if (print_lint(model_path, t)) { printf("skipped: fix the problems above first\n"); continue; }
        char c = ask_review();
        if (c == 'q') break;
        if (c == 'a') { approve(t); approved++; printf("%s approved\n", t->id); }
    }
    if (open) printf("\n%d approved\n", approved);
    Ticket *cur = ticket_current(ts);
    if (cur) {
        printf("%s is %s; the queue continues when it is done\n", cur->id, cur->status);
        return 0;
    }
    return start_next(ts, model_path);
}

/* ---- drafts: `ticket gen` and `ticket fill` ---- */

/* A draft becomes open once it has a title and a goal and passes the model
 * checks. Returns 1 when it is open now. */
static int promote_draft(Ticket *t, const char *model_path) {
    int missing = 0;
    if (!t->title || !*t->title) { fprintf(stderr, "  ticket %s needs a title\n", t->id); missing = 1; }
    if (!t->goal || !*t->goal) { fprintf(stderr, "  ticket %s needs a goal\n", t->id); missing = 1; }
    if (missing || print_lint(model_path, t)) {
        printf("%-10s draft: fill it in with stategate ticket fill %s --title \"...\" --goal \"...\"\n", t->id, t->id);
        return 0;
    }
    t->status = xstrdup("open");
    ticket_save(t);
    printf("%-10s open: ready for the architect\n", t->id);
    return 1;
}

static int has_stub_marker(const char *path) {
    char *text = read_file(path, NULL);
    int r = text && strstr(text, "// @stub") != NULL;
    free(text);
    return r;
}

/* Next free id of the form T001, T002, ... */
static void next_id(const Tickets *ts, const StrList *made, char out[32]) {
    int max = 0;
    for (int i = 0; i < ts->n; i++)
        if (ts->v[i].id[0] == 'T' && isdigit((unsigned char)ts->v[i].id[1])) {
            int n = atoi(ts->v[i].id + 1);
            if (n > max) max = n;
        }
    for (int i = 0; i < made->n; i++) {
        int n = atoi(made->v[i] + 1);
        if (n > max) max = n;
    }
    snprintf(out, 32, "T%03d", max + 1);
}

/* ticket gen: one draft per action or service that is still a stub and has
 * no ticket for the active step yet (split parents don't count). Deterministic, no AI: it fills in the node, step and
 * files and leaves the title and goal for the ticket writer. */
static int ticket_gen(Tickets *ts, const char *model_path) {
    if (!need_model(model_path)) return 2;
    SgModel m;
    memset(&m, 0, sizeof m);
    StrList e = {0};
    if (model_load(model_path, &m, &e) != 0) {
        for (int i = 0; i < e.n; i++) fprintf(stderr, "%s\n", e.v[i]);
        return 1;
    }
    const char *step = NULL;
    for (int i = 0; i < m.n_steps; i++)
        if (!strcmp(m.steps[i].status, "active")) { step = m.steps[i].id; break; }
    if (!step) { fprintf(stderr, "the model has no active step; mark one step status: active\n"); return 1; }

    StrList nodes = {0}, paths = {0};
    char path[512];
    for (int i = 0; i < m.n_actions; i++) {
        snprintf(path, sizeof path, "src/actions/%s.rs", m.actions[i].id);
        if (m.actions[i].stub || has_stub_marker(path)) { sl_push(&nodes, m.actions[i].id); sl_push(&paths, path); }
    }
    for (int i = 0; i < m.n_services; i++) {
        snprintf(path, sizeof path, "src/services/%s.rs", m.services[i].id);
        if (m.services[i].stub || has_stub_marker(path)) { sl_push(&nodes, m.services[i].id); sl_push(&paths, path); }
    }
    mkdirs(TICKETS_DIR);
    StrList made = {0};
    int skipped = 0;
    for (int k = 0; k < nodes.n; k++) {
        int covered = 0;
        for (int i = 0; i < ts->n; i++)
            if (strcmp(ts->v[i].status, "split") && !strcmp(ts->v[i].step, step) && sl_contains(&ts->v[i].nodes, nodes.v[k]))
                covered = 1;
        if (covered) { skipped++; continue; }
        char id[32], file[512], test[512];
        next_id(ts, &made, id);
        snprintf(file, sizeof file, "%s/%s.yaml", TICKETS_DIR, id);
        snprintf(test, sizeof test, "tests/logic_%s.rs", nodes.v[k]);
        Ticket t;
        memset(&t, 0, sizeof t);
        t.id = id;
        t.file = file;
        t.title = "";
        t.goal = "";
        t.step = (char *)step;
        t.status = "draft";
        t.base = "";
        t.approval = "";
        sl_push(&t.nodes, nodes.v[k]);
        sl_push(&t.files, paths.v[k]);
        sl_push(&t.files, test);
        if (ticket_save(&t) != 0) { fprintf(stderr, "cannot write %s\n", file); return 1; }
        sl_push(&made, id);
        printf("  draft %s  %s  (%s, %s)\n", id, nodes.v[k], paths.v[k], test);
    }
    printf("%d draft(s) written", made.n);
    if (skipped) printf(", %d node(s) already have a ticket for %s", skipped, step);
    printf(".%s\n", made.n ? " Next: the ticket writer fills each with stategate ticket fill <id> --title \"...\" --goal \"...\"" : "");
    return 0;
}

/* ticket fill <id> --title T --goal G: the ticket writer's only edit. It
 * cannot touch the node, step or files that `ticket gen` chose. */
static int ticket_fill(int argc, char **argv, Tickets *ts, const char *model_path) {
    if (!need_model(model_path)) return 2;
    Ticket *t = ticket_find(ts, argv[1]);
    if (!t) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }
    if (strcmp(t->status, "draft") && strcmp(t->status, "open")) {
        fprintf(stderr, "ticket %s is %s; only draft and open tickets can be filled in\n", t->id, t->status);
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", argv[i]); return 2; }
        if (!strcmp(argv[i], "--title")) t->title = argv[++i];
        else if (!strcmp(argv[i], "--goal")) t->goal = argv[++i];
        else { fprintf(stderr, "unknown option %s (fill takes --title and --goal)\n", argv[i]); return 2; }
    }
    ticket_save(t);
    if (!strcmp(t->status, "draft")) return promote_draft(t, model_path) ? 0 : 1;
    printf("%-10s updated\n", t->id);
    return 0;
}

/* ---- the implementer's loop: `ticket prompt` and `ticket run` ---- */

static void print_file_block(const char *path) {
    char *text = read_file(path, NULL);
    if (!text) { printf("\n### %s\n\n(does not exist yet; create it)\n", path); return; }
    size_t n = strlen(text);
    printf("\n### %s\n\n```rust\n%s%s```\n", path, text, n && text[n - 1] != '\n' ? "\n" : "");
    free(text);
}

static void print_node_context(const SgModel *m, const char *id) {
    Action *a = model_action(m, id);
    Service *sv = model_service(m, id);
    if (a) printf("- `%s` is an action%s%s%s\n", id, a->locked ? " (locked: a safety item, never stub or weaken it)" : "",
                  a->note ? ". Note: " : "", a->note ? a->note : "");
    if (sv) {
        printf("- `%s` is a service (fidelity %s); it may emit:", id, sv->fidelity ? sv->fidelity : "?");
        for (int k = 0; k < sv->emits.n; k++) printf(" %s", sv->emits.v[k]);
        printf("%s%s\n", sv->note ? ". Note: " : "", sv->note ? sv->note : "");
    }
    for (int i = 0; i < m->n_states; i++) {
        const State *s = &m->states[i];
        if (sl_contains(&s->entry, id)) printf("- runs on entry to %s\n", s->id);
        if (sl_contains(&s->exit, id)) printf("- runs on exit from %s\n", s->id);
        if (sl_contains(&s->invoke, id)) printf("- runs while in %s\n", s->id);
    }
    if (model_state(m, id)) printf("- `%s` is a state\n", id);
}

/* ticket prompt [id]: everything an AI needs for the ticket in one block of
 * Markdown, so any assistant can do the work from a paste. */
static int ticket_prompt(Tickets *ts, const char *id, const char *model_path) {
    if (!need_model(model_path)) return 2;
    Ticket *t = id ? ticket_find(ts, id) : ticket_current(ts);
    if (!t) {
        fprintf(stderr, id ? "no ticket %s\n" : "no active ticket: the architect approves one with stategate ticket start\n", id);
        return 1;
    }
    SgModel m;
    memset(&m, 0, sizeof m);
    StrList e = {0};
    model_load(model_path, &m, &e);
    const char *step_goal = "";
    for (int i = 0; i < m.n_steps; i++)
        if (!strcmp(m.steps[i].id, t->step)) step_goal = m.steps[i].goal;

    printf("# Ticket %s: %s\n\n", t->id, t->title);
    if (!strcmp(t->status, "locked")) {
        printf("This ticket is LOCKED after %d failed gate runs. Do not change code. Propose to the\n"
               "architect how to split it into smaller tickets.\n\n", t->failures);
    } else if (strcmp(t->status, "active")) {
        printf("Status: %s. Only an active ticket may be worked on.\n\n", t->status);
    }
    printf("## Goal\n\n%s\n\n", *t->goal ? t->goal : "(no goal written)");
    printf("Step %s: %s\n\n", t->step, step_goal);
    printf("## Nodes\n\n");
    for (int i = 0; i < t->nodes.n; i++) print_node_context(&m, t->nodes.v[i]);
    printf("\n## Files you may change\n\n");
    for (int i = 0; i < t->files.n; i++) printf("- `%s`\n", t->files.v[i]);
    printf("- `CHANGELOG.md`: add `## %s` naming", t->id);
    for (int i = 0; i < t->nodes.n; i++) printf(" %s", t->nodes.v[i]);
    printf("\n\nChange nothing else: not generated files, not `%s`, not `.fsm/`.\n", model_path);
    printf("Keep the `@action`/`@service`/`@node` tags. Remove `// @stub` once the logic is real.\n");
    printf("Node logic receives `SafeHal`, which enforces the safety limits.\n");
    if (t->errors.n) {
        printf("\n## The last gate run failed\n\nFix this first:\n\n```\n");
        for (int i = 0; i < t->errors.n; i++) printf("%s\n", t->errors.v[i]);
        printf("```\n\nFailed runs so far: %d of %d.\n", t->failures, MAX_FAILURES);
    }
    printf("\n## Current code\n");
    for (int i = 0; i < t->files.n; i++) {
        const char *f = t->files.v[i];
        if (!strpbrk(f, "*?[")) { print_file_block(f); continue; }
        int st;
        char cmd[600];
        snprintf(cmd, sizeof cmd, "git ls-files --cached --others --exclude-standard -- '%s' 2>/dev/null", f);
        char *list = run_capture(cmd, &st);
        char *save = NULL;
        for (char *p = strtok_r(list, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) print_file_block(p);
    }
    printf("\n## When you are done\n\nRun `stategate ticket run` and report its output as it is. Green finishes the\n"
           "ticket. Red saves the errors into the ticket: run `stategate ticket prompt` again and fix them.\n");
    return 0;
}

/* ticket run: the gate for the active ticket. Green commits the work, marks
 * the ticket done and starts the next approved one. Red keeps the ticket
 * active with the errors saved in it (the gate does that), until it locks. */
static int ticket_run(const char *model_path) {
    if (!need_model(model_path)) return 2;
    StrList errors = {0};
    Tickets ts;
    tickets_load(&ts, &errors);
    Ticket *t = ticket_current(&ts);
    if (!t) { fprintf(stderr, "no active ticket: the architect approves one with stategate ticket start\n"); return 1; }
    if (!strcmp(t->status, "locked")) {
        fprintf(stderr, "ticket %s is locked. Propose a split; the architect runs stategate ticket split %s \"...\" \"...\"\n", t->id, t->id);
        return 1;
    }
    char id[128];
    snprintf(id, sizeof id, "%s", t->id);
    int red = cmd_gate(model_path, 0);
    tickets_load(&ts, &errors);
    t = ticket_find(&ts, id);
    if (red) {
        if (t && !strcmp(t->status, "locked"))
            printf("\n%s is locked. Propose a split to the architect.\n", id);
        else
            printf("\n%s stays active with the errors saved. Next: stategate ticket prompt, fix, stategate ticket run\n", id);
        return 1;
    }
    t->status = xstrdup("done");
    ticket_save(t);
    char msg[600], cmd[800];
    snprintf(msg, sizeof msg, "%s: %s", t->id, t->title);
    for (char *c = msg; *c; c++) if (*c == '\'') *c = ' ';
    snprintf(cmd, sizeof cmd, "git add -A -- . && git commit -q -m '%s' 2>&1", msg);
    int st;
    char *out = run_capture(cmd, &st);
    if (st != 0) {
        t->status = xstrdup("active");
        ticket_save(t);
        fprintf(stderr, "the gate is green but git commit failed, so %s stays active:\n%s", t->id, out);
        return 1;
    }
    printf("\n%s done and committed.\n", t->id);
    return start_next(&ts, model_path);
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
            printf("%-10s %-8s fails %d/%d  %s\n", ts.v[i].id, ts.v[i].status, ts.v[i].failures, MAX_FAILURES,
                   ts.v[i].title);
        return 0;
    }
    if (!strcmp(sub, "check")) {
        if (!need_model(model_path)) return 2;
        int bad = 0, n = 0;
        for (int i = 0; i < ts.n; i++) {
            Ticket *c = &ts.v[i];
            int pick = argc >= 2 ? !strcmp(c->id, argv[1]) : (!strcmp(c->status, "open") || !strcmp(c->status, "draft"));
            if (!pick) continue;
            n++;
            if (!strcmp(c->status, "draft")) {
                if (!promote_draft(c, model_path)) bad = 1;
            } else if (print_lint(model_path, c)) { printf("%-10s problems above\n", c->id); bad = 1; }
            else printf("%-10s ok\n", c->id);
        }
        if (argc >= 2 && !n) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }
        if (!n) printf("no draft or open tickets\n");
        return bad;
    }
    if (!strcmp(sub, "gen")) return ticket_gen(&ts, model_path);
    if (!strcmp(sub, "prompt")) return ticket_prompt(&ts, argc >= 2 ? argv[1] : NULL, model_path);
    if (!strcmp(sub, "run")) return ticket_run(model_path);
    if (!strcmp(sub, "start") && argc < 2) return review_open(&ts, model_path);
    if (!strcmp(sub, "next")) {
        Ticket *cur = ticket_current(&ts);
        if (cur) { fprintf(stderr, "ticket %s is %s; finish or split it first\n", cur->id, cur->status); return 1; }
        return start_next(&ts, model_path);
    }
    if (argc < 2) return ticket_usage();
    if (!strcmp(sub, "new")) return ticket_new(argc, argv, &ts, model_path);
    if (!strcmp(sub, "fill")) return ticket_fill(argc, argv, &ts, model_path);
    Ticket *t = ticket_find(&ts, argv[1]);
    if (!t) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }

    if (!strcmp(sub, "start")) {
        Ticket *cur = ticket_current(&ts);
        if (cur && cur != t) { fprintf(stderr, "ticket %s is %s; finish or split it first\n", cur->id, cur->status); return 1; }
        if (!strcmp(t->status, "approved")) return activate(t, model_path) ? 0 : 1;
        if (strcmp(t->status, "open")) { fprintf(stderr, "ticket %s is %s, not open\n", t->id, t->status); return 1; }
        if (!need_model(model_path)) return 2;
        if (print_lint(model_path, t)) { fprintf(stderr, "ticket %s cannot be started; fix it first\n", t->id); return 1; }
        printf("Approve this ticket? An agent will work on it, limited to these files.\n\n");
        print_ticket(t);
        printf("\n");
        if (!confirm_by_typing("approve")) { fprintf(stderr, "not approved; %s stays open\n", t->id); return 1; }
        approve(t);
        return activate(t, model_path) ? 0 : 1;
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
        return start_next(&ts, model_path);
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
