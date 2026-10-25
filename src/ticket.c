#include "ticket.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ydoc.h"

static const char *const TICKET_KEYS[] = {"id", "title", "step", "status", "base", "failures",
                                          "nodes", "files", NULL};

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
        t->failures = (int)y_int(&r, y_get(&r, root, "failures"), "failures", 0);
        y_strlist(&r, y_get(&r, root, "nodes"), "nodes", &t->nodes);
        y_strlist(&r, y_get(&r, root, "files"), "files", &t->files);
        if (!t->id) { y_err(&r, root, "ticket is missing 'id'"); t->id = xstrdup("?"); }
        if (!t->title) t->title = xstrdup("");
        if (!t->step) t->step = xstrdup("");
        if (!t->status) t->status = xstrdup("open");
        if (!t->base) t->base = xstrdup("");
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
    sb_printf(&b, "id: %s\ntitle: \"%s\"\nstep: %s\nstatus: %s\nbase: \"%s\"\nfailures: %d\n", t->id, t->title,
              *t->step ? t->step : "\"\"", t->status, t->base, t->failures);
    yaml_list(&b, "nodes", &t->nodes);
    yaml_list(&b, "files", &t->files);
    return write_file(t->file, b.s, b.len);
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
            "       stategate ticket start <id>\n"
            "       stategate ticket done <id>\n"
            "       stategate ticket split <id> \"<sub-step title>\" \"<sub-step title>\" ...\n");
    return 2;
}

int cmd_ticket(int argc, char **argv) {
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
    if (argc < 2) return ticket_usage();
    Ticket *t = ticket_find(&ts, argv[1]);
    if (!t) { fprintf(stderr, "no ticket %s\n", argv[1]); return 1; }

    if (!strcmp(sub, "start")) {
        Ticket *cur = ticket_current(&ts);
        if (cur && cur != t) { fprintf(stderr, "ticket %s is %s; finish or split it first\n", cur->id, cur->status); return 1; }
        if (strcmp(t->status, "open")) { fprintf(stderr, "ticket %s is %s, not open\n", t->id, t->status); return 1; }
        int st;
        char *head = run_capture("git rev-parse HEAD 2>/dev/null", &st);
        head[strcspn(head, "\n")] = 0;
        if (st != 0 || !*head) { fprintf(stderr, "ticket start needs a git repository with at least one commit\n"); return 1; }
        t->base = head;
        t->status = xstrdup("active");
        t->failures = 0;
        ticket_save(t);
        printf("%s active from %.12s. Allowed files:\n", t->id, t->base);
        for (int i = 0; i < t->files.n; i++) printf("  %s\n", t->files.v[i]);
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
