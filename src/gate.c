#include "gate.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gen_rust.h"
#include "manifest.h"
#include "ticket.h"
#include "validate.h"

#define SIGNOFF_PATH ".fsm/signoff.lock"
#define GATE_JSON ".fsm/gate.json"
#define CHANGELOG "CHANGELOG.md"


static void print_errors(const char *title, const StrList *e) {
    if (!e->n) return;
    fprintf(stderr, "%s:\n", title);
    for (int i = 0; i < e->n; i++) fprintf(stderr, "  %s\n", e->v[i]);
}

int cmd_validate(const char *model_path) {
    SgModel m;
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    int r = validate_all(model_path, &m, &e1, &e2, &e3);
    print_errors("1 schema", &e1);
    print_errors("2 statechart", &e2);
    print_errors("3 invariants", &e3);
    if (r == 0) printf("%s is valid: %d states, %d events, %d invariants\n", model_path, m.n_states, m.events.n, m.n_invariants);
    return r == 0 ? 0 : 1;
}

int cmd_gen(const char *model_path) {
    SgModel m;
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    if (validate_all(model_path, &m, &e1, &e2, &e3) != 0) {
        print_errors("1 schema", &e1);
        print_errors("2 statechart", &e2);
        print_errors("3 invariants", &e3);
        fprintf(stderr, "not generating from an invalid model\n");
        return 1;
    }
    GenSet g;
    gen_rust(&m, &g);
    int wrote = 0, scaffolded = 0;
    for (int i = 0; i < g.n; i++) {
        GenFile *f = &g.v[i];
        if (f->scaffold && file_exists(f->path)) continue;
        if (write_file(f->path, f->content, strlen(f->content)) != 0) {
            fprintf(stderr, "cannot write %s\n", f->path);
            return 1;
        }
        if (f->scaffold) {
            printf("  scaffold  %s\n", f->path);
            scaffolded++;
        } else {
            wrote++;
        }
    }
    if (manifest_write(&m, &g) != 0) { fprintf(stderr, "cannot write %s\n", MANIFEST_PATH); return 1; }
    printf("generated %d files, scaffolded %d, manifest %s\n", wrote, scaffolded, MANIFEST_PATH);
    return 0;
}

char *locked_canonical(const SgModel *m) {
    SB b;
    sb_init(&b);
    for (int i = 0; i < m->n_actions; i++) {
        const Action *a = &m->actions[i];
        if (!a->locked) continue;
        sb_printf(&b, "action %s stub=%d used_by=", a->id, a->stub);
        for (int s = 0; s < m->n_states; s++) {
            if (sl_contains(&m->states[s].entry, a->id)) sb_printf(&b, "%s:entry,", m->states[s].id);
            if (sl_contains(&m->states[s].exit, a->id)) sb_printf(&b, "%s:exit,", m->states[s].id);
        }
        sb_puts(&b, "\n");
    }
    for (int i = 0; i < m->n_services; i++) {
        const Service *s = &m->services[i];
        if (!s->locked) continue;
        sb_printf(&b, "service %s stub=%d emits=", s->id, s->stub);
        for (int k = 0; k < s->emits.n; k++) sb_printf(&b, "%s,", s->emits.v[k]);
        sb_puts(&b, "\n");
    }
    for (int i = 0; i < m->n_invariants; i++) {
        const Invariant *v = &m->invariants[i];
        if (!v->locked) continue;
        const HalCall *h = v->call ? model_hal(m, v->call) : NULL;
        sb_printf(&b, "invariant %s kind=%s event=%s target=%s state=%s exit_event=%s limit_ms=%ld call=%s arg=%s max=%ld\n",
                  v->id, v->kind_name, v->event ? v->event : "", v->target ? v->target : "", v->state ? v->state : "",
                  v->exit_event ? v->exit_event : "", v->limit_ms, v->call ? v->call : "", h ? h->arg : "", v->max);
    }
    return b.s;
}

static char *locked_hash(const SgModel *m) {
    char *c = locked_canonical(m);
    char hex[65];
    sha256_hex(c, strlen(c), hex);
    free(c);
    return xstrdup(hex);
}

int cmd_signoff(const char *model_path) {
    SgModel m;
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    if (validate_all(model_path, &m, &e1, &e2, &e3) != 0) {
        print_errors("1 schema", &e1);
        print_errors("2 statechart", &e2);
        print_errors("3 invariants", &e3);
        fprintf(stderr, "refusing to sign off an invalid model\n");
        return 1;
    }
    char *canon = locked_canonical(&m);
    if (!*canon) {
        printf("Nothing in %s is locked, so there is nothing to sign off.\n", model_path);
        return 0;
    }
    printf("Locked items in %s:\n\n%s\n", model_path, canon);
    char *old = read_file(SIGNOFF_PATH, NULL);
    char *hash = locked_hash(&m);
    if (old && strstr(old, hash)) { printf("Already signed off; nothing changed.\n"); return 0; }
    if (!confirm_by_typing("sign off")) { fprintf(stderr, "not signed off\n"); return 1; }
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    SB b;
    sb_init(&b);
    sb_printf(&b, "# Written by `stategate signoff`. Only the architect runs it.\nhash: %s\nsigned: %s\n\n", hash, when);
    char *save = NULL;
    for (char *line = strtok_r(canon, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        sb_printf(&b, "# %s\n", line);
    if (write_file(SIGNOFF_PATH, b.s, b.len) != 0) { fprintf(stderr, "cannot write %s\n", SIGNOFF_PATH); return 1; }
    printf("signed off: %s\n", SIGNOFF_PATH);
    return 0;
}

/* ---- the gate ---- */

enum { C_SCHEMA, C_STATECHART, C_INVARIANTS, C_SIGNOFF, C_DRIFT, C_WILDCARD, C_TRACE,
       C_STUBS, C_SCOPE, C_CHANGELOG, C_TESTS, N_CHECKS };

static const char *CHECK_NAMES[N_CHECKS] = {
    "schema", "statechart", "invariants", "signoff", "drift", "no_wildcards",
    "trace_tags", "stubs", "ticket_scope", "changelog", "tests",
};

typedef struct {
    const char *status; /* pass | fail | skip */
    StrList msgs;
} CheckResult;

static void json_str(SB *b, const char *s) {
    sb_puts(b, "\"");
    for (const char *c = s; *c; c++) {
        if (*c == '"' || *c == '\\') sb_printf(b, "\\%c", *c);
        else if (*c == '\n') sb_puts(b, "\\n");
        else if (*c == '\t') sb_puts(b, "\\t");
        else if ((unsigned char)*c < 0x20) sb_printf(b, "\\u%04x", *c);
        else sb_printf(b, "%c", *c);
    }
    sb_puts(b, "\"");
}

static int contains_tag(const char *text, const char *tag, const char *value) {
    /* Matches "@tag value" followed by a space, newline or end. */
    char needle[512];
    snprintf(needle, sizeof needle, "@%s %s", tag, value);
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) {
        char after = p[strlen(needle)];
        if (after == 0 || after == ' ' || after == '\n' || after == '\r') return 1;
    }
    return 0;
}

static void check_drift(const SgModel *m, const GenSet *g, StrList *e) {
    Manifest mf;
    if (manifest_read(&mf) != 0) {
        sl_pushf(e, "%s is missing: run stategate gen", MANIFEST_PATH);
        return;
    }
    if (strcmp(mf.model_hash, m->hash)) {
        sl_pushf(e, "the model changed since the last generation: run stategate gen");
        return;
    }
    for (int i = 0; i < g->n; i++) {
        const GenFile *f = &g->v[i];
        if (f->scaffold) continue;
        size_t len;
        char *disk = read_file(f->path, &len);
        if (!disk) { sl_pushf(e, "%s is missing: run stategate gen", f->path); continue; }
        char hex[65], want[65];
        sha256_hex(disk, len, hex);
        sha256_hex(f->content, strlen(f->content), want);
        const char *recorded = manifest_hash_of(&mf, f->path);
        if (recorded && strcmp(hex, recorded))
            sl_pushf(e, "%s was edited by hand (its hash differs from the manifest). Generated files are never edited; run stategate gen to restore it", f->path);
        else if (strcmp(hex, want))
            sl_pushf(e, "%s does not match what this version of stategate generates: run stategate gen", f->path);
        free(disk);
    }
}

static void check_wildcards(const GenSet *g, StrList *e) {
    for (int i = 0; i < g->n; i++) {
        const GenFile *f = &g->v[i];
        if (f->scaffold) continue;
        size_t n = strlen(f->path);
        if (n < 3 || strcmp(f->path + n - 3, ".rs")) continue;
        char *disk = read_file(f->path, NULL);
        if (!disk) continue;
        int line = 1;
        for (char *p = disk; *p; p++) {
            if (*p == '\n') line++;
            if (p[0] == '_' && p[1] == ' ' && p[2] == '=' && p[3] == '>' && (p == disk || p[-1] == ' ' || p[-1] == '|'))
                sl_pushf(e, "%s:%d: wildcard match arm '_ =>' in generated code", f->path, line);
        }
        free(disk);
    }
}

static void check_traces(const SgModel *m, StrList *e) {
    char path[512];
    for (int i = 0; i < m->n_actions; i++) {
        const Action *a = &m->actions[i];
        snprintf(path, sizeof path, "src/actions/%s.rs", a->id);
        char *text = read_file(path, NULL);
        if (!text) { sl_pushf(e, "%s is missing (action %s): run stategate gen to scaffold it", path, a->id); continue; }
        if (!contains_tag(text, "action", a->id)) sl_pushf(e, "%s: missing trace tag '// @action %s'", path, a->id);
        for (int s = 0; s < m->n_states; s++) {
            const State *st = &m->states[s];
            if ((sl_contains(&st->entry, a->id) || sl_contains(&st->exit, a->id)) && !contains_tag(text, "node", st->id))
                sl_pushf(e, "%s: missing trace tag '// @node %s'", path, st->id);
        }
        free(text);
    }
    for (int i = 0; i < m->n_services; i++) {
        const Service *sv = &m->services[i];
        snprintf(path, sizeof path, "src/services/%s.rs", sv->id);
        char *text = read_file(path, NULL);
        if (!text) { sl_pushf(e, "%s is missing (service %s): run stategate gen to scaffold it", path, sv->id); continue; }
        if (!contains_tag(text, "service", sv->id)) sl_pushf(e, "%s: missing trace tag '// @service %s'", path, sv->id);
        for (int s = 0; s < m->n_states; s++)
            if (sl_contains(&m->states[s].invoke, sv->id) && !contains_tag(text, "node", m->states[s].id))
                sl_pushf(e, "%s: missing trace tag '// @node %s'", path, m->states[s].id);
        free(text);
    }
}

static int file_has_stub_marker(const char *path) {
    char *text = read_file(path, NULL);
    int r = text && strstr(text, "// @stub") != NULL;
    free(text);
    return r;
}

static int check_stubs(const SgModel *m, int release, StrList *e, StrList *stubs) {
    char path[512];
    for (int i = 0; i < m->n_actions; i++) {
        const Action *a = &m->actions[i];
        snprintf(path, sizeof path, "src/actions/%s.rs", a->id);
        int marker = file_has_stub_marker(path);
        if (a->locked && (a->stub || marker))
            sl_pushf(e, "locked action %s is a stub; locked safety items are never stubbed (implement %s)", a->id, path);
        else if (!a->stub && marker)
            sl_pushf(e, "%s still carries '// @stub' but the model says action %s is implemented", path, a->id);
        if (a->stub || marker) sl_pushf(stubs, "action %s", a->id);
    }
    for (int i = 0; i < m->n_services; i++) {
        const Service *s = &m->services[i];
        snprintf(path, sizeof path, "src/services/%s.rs", s->id);
        int marker = file_has_stub_marker(path);
        if (s->locked && (s->stub || marker))
            sl_pushf(e, "locked service %s is a stub; locked safety items are never stubbed", s->id);
        else if (!s->stub && marker)
            sl_pushf(e, "%s still carries '// @stub' but the model says service %s is implemented", path, s->id);
        if (s->stub || marker) sl_pushf(stubs, "service %s", s->id);
    }
    if (release && stubs->n) {
        for (int i = 0; i < stubs->n; i++) sl_pushf(e, "release blocked: %s is still a stub", stubs->v[i]);
    }
    return stubs->n;
}

static int matches_any(const StrList *globs, const char *path) {
    for (int i = 0; i < globs->n; i++)
        if (fnmatch(globs->v[i], path, FNM_PATHNAME) == 0) return 1;
    return 0;
}

/* What a ticket may ask for, judged against the model. Shared by the gate
 * and by `ticket new|check|start`, so a ticket writer gets the same answer
 * the gate will give later. */
static void lint_ticket(const SgModel *m, const GenSet *g, const Ticket *t, StrList *e) {
    if (!t->title || !*t->title) sl_pushf(e, "ticket %s has no title", t->id);
    if (!t->step || !*t->step) sl_pushf(e, "ticket %s names no step", t->id);
    else {
        int found = 0;
        for (int i = 0; i < m->n_steps; i++) if (!strcmp(m->steps[i].id, t->step)) found = 1;
        if (!found) sl_pushf(e, "ticket %s names step '%s', which is not in the model", t->id, t->step);
    }
    if (!t->nodes.n) sl_pushf(e, "ticket %s names no nodes", t->id);
    for (int i = 0; i < t->nodes.n; i++) {
        const char *n = t->nodes.v[i];
        if (!model_state(m, n) && !model_action(m, n) && !model_service(m, n))
            sl_pushf(e, "ticket %s names node '%s', which is not a state, action or service in the model", t->id, n);
    }
    if (!t->files.n) sl_pushf(e, "ticket %s lists no files", t->id);
    StrList protected = {0};
    for (int i = 0; i < g->n; i++)
        if (!g->v[i].scaffold) sl_push(&protected, g->v[i].path);
    sl_push(&protected, m->path);
    sl_push(&protected, MANIFEST_PATH);
    sl_push(&protected, SIGNOFF_PATH);
    for (int i = 0; i < t->files.n; i++) {
        if (!strncmp(t->files.v[i], ".fsm", 4)) sl_pushf(e, "ticket %s grants '%s'; tickets may not grant .fsm/", t->id, t->files.v[i]);
        for (int k = 0; k < protected.n; k++)
            if (fnmatch(t->files.v[i], protected.v[k], FNM_PATHNAME) == 0)
                sl_pushf(e, "ticket %s grants '%s', which covers protected file %s", t->id, t->files.v[i], protected.v[k]);
    }
}

int ticket_lint(const char *model_path, const Ticket *t, StrList *errors) {
    int before = errors->n;
    SgModel m;
    memset(&m, 0, sizeof m);
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    if (validate_all(model_path, &m, &e1, &e2, &e3) != 0) {
        sl_pushf(errors, "the model %s does not validate; run stategate validate", model_path);
        return errors->n - before;
    }
    GenSet g = {0};
    gen_rust(&m, &g);
    lint_ticket(&m, &g, t, errors);
    return errors->n - before;
}

static void check_scope(const SgModel *m, const GenSet *g, const Ticket *t, StrList *e, StrList *notes) {
    if (!t) {
        sl_push(notes, "no active ticket: scope not checked (architect mode)");
        return;
    }
    lint_ticket(m, g, t, e);
    if (!t->base || !*t->base || !t->approval || !*t->approval) {
        sl_pushf(e, "ticket %s was never approved: the architect runs stategate ticket start %s at a terminal", t->id, t->id);
        return;
    }
    char want[65];
    ticket_approval_hash(t, want);
    if (strcmp(want, t->approval)) {
        sl_pushf(e, "ticket %s changed after the architect approved it (its step, base, nodes or files). Put it back, or the architect approves a new ticket", t->id);
        return;
    }

    char cmd[512];
    int st;
    snprintf(cmd, sizeof cmd, "git diff --name-only --relative %s -- . 2>&1", t->base);
    char *changed = run_capture(cmd, &st);
    if (st != 0) { sl_pushf(e, "git diff against ticket base %.12s failed: %s", t->base, changed); return; }
    char *untracked = run_capture("git ls-files --others --exclude-standard -- . 2>/dev/null", &st);
    SB all;
    sb_init(&all);
    sb_puts(&all, changed);
    sb_puts(&all, "\n");
    sb_puts(&all, untracked);

    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(all.s, "\n", &save); p; p = strtok_r(NULL, "\n", &save)) {
        if (!*p) continue;
        n++;
        /* Ticket files change as the queue moves (done, next). Each is
         * guarded by its approval hash instead: the active one here, the
         * queued ones when they start. */
        if (!strcmp(p, CHANGELOG) || !strncmp(p, TICKETS_DIR "/", strlen(TICKETS_DIR) + 1)) continue;
        if (!matches_any(&t->files, p)) sl_pushf(e, "%s is outside ticket %s's scope", p, t->id);
    }
    sl_pushf(notes, "%d changed file(s) since %.12s", n, t->base);
}

static void check_changelog(const Ticket *t, StrList *e, StrList *notes) {
    if (!t) { sl_push(notes, "no active ticket: changelog not checked"); return; }
    char *text = read_file(CHANGELOG, NULL);
    if (!text) { sl_pushf(e, "%s is missing; add an entry '## %s' naming the changed node IDs", CHANGELOG, t->id); return; }
    /* Find the "## <id>" section and require every ticket node ID in it. */
    char heading[256];
    snprintf(heading, sizeof heading, "## %s", t->id);
    char *sec = NULL;
    for (char *p = strstr(text, heading); p; p = strstr(p + 1, heading)) {
        char after = p[strlen(heading)];
        if ((p == text || p[-1] == '\n') && (after == '\n' || after == ' ' || after == ':' || after == 0)) { sec = p; break; }
    }
    if (!sec) { sl_pushf(e, "%s has no entry '## %s'", CHANGELOG, t->id); return; }
    char *end = strstr(sec + 3, "\n## ");
    if (end) *end = 0;
    for (int i = 0; i < t->nodes.n; i++)
        if (!strstr(sec, t->nodes.v[i])) sl_pushf(e, "%s entry for %s does not mention node %s", CHANGELOG, t->id, t->nodes.v[i]);
    free(text);
}

static void check_tests(StrList *e, StrList *notes) {
    int st;
    char *out = run_capture("cargo test --quiet --color never 2>&1", &st);
    /* Keep the summary lines, and the tail when something failed. */
    int total_passed = 0, total_failed = 0;
    for (char *p = strstr(out, "test result:"); p; p = strstr(p + 1, "test result:")) {
        int a = 0, b = 0;
        char *q = strstr(p, ". ");
        if (q && sscanf(q, ". %d passed; %d failed", &a, &b) == 2) { total_passed += a; total_failed += b; }
    }
    if (st != 0) {
        sl_pushf(e, "cargo test failed (exit %d): %d passed, %d failed", st, total_passed, total_failed);
        /* last ~25 lines */
        int lines = 0;
        char *p = out + strlen(out);
        while (p > out && lines < 25) { p--; if (*p == '\n') lines++; }
        sl_push(e, p);
    } else {
        sl_pushf(notes, "%d tests passed", total_passed);
    }
    free(out);
}

static void write_gate_json(int ok, int release, const SgModel *m, const Ticket *t, int stub_count, const StrList *stubs,
                            CheckResult *res) {
    SB b;
    sb_init(&b);
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    sb_printf(&b, "{\n  \"ok\": %s,\n  \"release\": %s,\n  \"time\": \"%s\",\n", ok ? "true" : "false",
              release ? "true" : "false", when);
    sb_puts(&b, "  \"model\": ");
    json_str(&b, m && m->path ? m->path : "");
    sb_printf(&b, ",\n  \"model_hash\": \"%s\",\n", m && m->hash ? m->hash : "");
    if (t) {
        sb_printf(&b, "  \"ticket\": \"%s\",\n  \"ticket_status\": \"%s\",\n  \"failures\": %d,\n  \"max_failures\": %d,\n",
                  t->id, t->status, t->failures, MAX_FAILURES);
    } else {
        sb_puts(&b, "  \"ticket\": null,\n");
    }
    sb_printf(&b, "  \"stub_count\": %d,\n  \"stubs\": [", stub_count);
    for (int i = 0; i < stubs->n; i++) {
        if (i) sb_puts(&b, ", ");
        json_str(&b, stubs->v[i]);
    }
    sb_puts(&b, "],\n");
    const char *active_step = "";
    if (m)
        for (int i = 0; i < m->n_steps; i++)
            if (!strcmp(m->steps[i].status, "active")) active_step = m->steps[i].id;
    sb_puts(&b, "  \"step\": ");
    json_str(&b, active_step);
    sb_puts(&b, ",\n  \"checks\": [\n");
    for (int i = 0; i < N_CHECKS; i++) {
        sb_printf(&b, "    { \"id\": %d, \"name\": \"%s\", \"status\": \"%s\", \"messages\": [", i + 1, CHECK_NAMES[i],
                  res[i].status);
        for (int k = 0; k < res[i].msgs.n; k++) {
            if (k) sb_puts(&b, ", ");
            json_str(&b, res[i].msgs.v[k]);
        }
        sb_printf(&b, "] }%s\n", i + 1 < N_CHECKS ? "," : "");
    }
    sb_puts(&b, "  ]\n}\n");
    write_file(GATE_JSON, b.s, b.len);
}

int cmd_gate(const char *model_path, int release) {
    CheckResult res[N_CHECKS];
    for (int i = 0; i < N_CHECKS; i++) { res[i].status = "skip"; memset(&res[i].msgs, 0, sizeof(StrList)); }

    StrList terrs = {0};
    Tickets ts;
    tickets_load(&ts, &terrs);
    Ticket *t = ticket_current(&ts);

    SgModel m;
    memset(&m, 0, sizeof m);
    int failed = -1;
    StrList stubs = {0}, notes[N_CHECKS];
    memset(notes, 0, sizeof notes);
    int stub_count = 0;

    if (terrs.n) {
        res[C_SCOPE].status = "fail";
        for (int i = 0; i < terrs.n; i++) sl_push(&res[C_SCOPE].msgs, terrs.v[i]);
        failed = C_SCOPE;
    } else if (t && !strcmp(t->status, "locked")) {
        res[C_SCOPE].status = "fail";
        sl_pushf(&res[C_SCOPE].msgs, "ticket %s is locked after %d failed gate runs. Stop and propose how to split it; the architect approves with: stategate ticket split %s \"...\" \"...\"",
                 t->id, t->failures, t->id);
        failed = C_SCOPE;
        model_load(model_path, &m, &terrs);
    }

    GenSet g = {0};
    for (int c = 0; c < N_CHECKS && failed < 0; c++) {
        StrList *e = &res[c].msgs;
        switch (c) {
        case C_SCHEMA: model_load(model_path, &m, e); break;
        case C_STATECHART: check_statechart(&m, e); break;
        case C_INVARIANTS: check_model_invariants(&m, e); gen_rust(&m, &g); break;
        case C_SIGNOFF: {
            char *have = read_file(SIGNOFF_PATH, NULL);
            char *want = locked_hash(&m);
            char *canon0 = locked_canonical(&m);
            if (!*canon0) { sl_push(&notes[c], "nothing is locked: no sign-off needed"); break; }
            if (!have) sl_pushf(e, "no sign-off yet: the architect runs stategate signoff");
            else if (!strstr(have, want)) {
                sl_pushf(e, "locked items changed since the architect signed off. Revert them, or the architect reviews and runs stategate signoff. Locked items now:");
                char *canon = locked_canonical(&m);
                char *save = NULL;
                for (char *l = strtok_r(canon, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) sl_pushf(e, "  %s", l);
            }
            break;
        }
        case C_DRIFT: check_drift(&m, &g, e); break;
        case C_WILDCARD: check_wildcards(&g, e); break;
        case C_TRACE: check_traces(&m, e); break;
        case C_STUBS: stub_count = check_stubs(&m, release, e, &stubs); break;
        case C_SCOPE: check_scope(&m, &g, t, e, &notes[c]); break;
        case C_CHANGELOG: check_changelog(t, e, &notes[c]); break;
        case C_TESTS: check_tests(e, &notes[c]); break;
        }
        if (e->n) { res[c].status = "fail"; failed = c; }
        else {
            res[c].status = "pass";
            for (int k = 0; k < notes[c].n; k++) sl_push(e, notes[c].v[k]);
        }
    }
    if (failed < 0 && stubs.n) {
        for (int k = 0; k < stubs.n; k++) sl_pushf(&res[C_STUBS].msgs, "stub: %s", stubs.v[k]);
    }

    int ok = failed < 0;
    if (!release && t && (!strcmp(t->status, "active") || !strcmp(t->status, "locked"))) {
        /* The ticket keeps what the last run said, so `ticket prompt` can
         * hand the errors to whichever AI fixes them. */
        int had = t->errors.n;
        memset(&t->errors, 0, sizeof t->errors);
        if (!ok) {
            sl_pushf(&t->errors, "check %d (%s) failed:", failed + 1, CHECK_NAMES[failed]);
            for (int k = 0; k < res[failed].msgs.n && k < 60; k++) sl_push(&t->errors, res[failed].msgs.v[k]);
            if (res[failed].msgs.n > 60) sl_pushf(&t->errors, "... %d more lines in .fsm/gate.json", res[failed].msgs.n - 60);
            if (!strcmp(t->status, "active")) {
                t->failures++;
                if (t->failures >= MAX_FAILURES) t->status = xstrdup("locked");
            }
        }
        if (!ok || had) ticket_save(t);
    }
    write_gate_json(ok, release, &m, t, stub_count, &stubs, res);

    printf("stategate gate%s  model %s", release ? " --release" : "", model_path);
    if (t) printf("  ticket %s (%d/%d failures)", t->id, t->failures, MAX_FAILURES);
    printf("\n");
    for (int i = 0; i < N_CHECKS; i++) {
        printf("  %s %2d %s\n", !strcmp(res[i].status, "pass") ? "PASS" : !strcmp(res[i].status, "fail") ? "FAIL" : "skip",
               i + 1, CHECK_NAMES[i]);
        for (int k = 0; k < res[i].msgs.n; k++) printf("         %s\n", res[i].msgs.v[k]);
    }
    if (ok) printf("GATE GREEN  stubs: %d\n", stub_count);
    else {
        printf("GATE RED  at check %d (%s)\n", failed + 1, CHECK_NAMES[failed]);
        if (t && !strcmp(t->status, "locked"))
            printf("Ticket %s is now locked. Do not retry: propose a split to the architect.\n", t->id);
    }
    return ok ? 0 : 1;
}

/* ---- init ---- */

static const char *AGENTS_MD =
    "# Rules for coding agents\n\n"
    "This project is built with stategate. The model file `%s` is the single source of\n"
    "truth: code is generated from it, and changes flow model to code only. A deterministic\n"
    "gate decides whether work lands. Your description of your work does not count;\n"
    "only `stategate gate` does.\n\n"
    "There are two agent roles. Do one per session and say which one you are doing.\n\n"
    "## Ticket writer\n\n"
    "You describe the work in tickets. You never write code.\n\n"
    "- `stategate ticket gen` (already run by the architect, or run it yourself) writes one\n"
    "  `draft` ticket per stub node with its node, step and files filled in.\n"
    "- For each draft, run `stategate ticket fill <id> --title \"...\" --goal \"...\"`. The goal\n"
    "  says what the node must do for this step, in a few sentences an implementer can test.\n"
    "  A filled draft becomes `open`. You cannot change a ticket's node, step or files.\n"
    "- For work `ticket gen` doesn't cover, write a ticket with `stategate ticket new <id>\n"
    "  --title \"...\" --goal \"...\" --step <step> --node <node id> --file <path or glob>`. Keep\n"
    "  it small: one behaviour, the fewest files, and the tests that prove it.\n"
    "- Run `stategate ticket check`, then stop. Only the architect approves tickets, at a\n"
    "  terminal with `stategate ticket start`. Never set a ticket's status, base or approval.\n\n"
    "## Implementer\n\n"
    "1. Run `stategate ticket prompt`. It prints the active ticket: goal, node, the files you\n"
    "   may change, their current code and the errors from the last failed run. Touch only\n"
    "   those files plus `CHANGELOG.md`. If there is no active ticket, stop.\n"
    "   When you are done, run `stategate ticket run`. Green commits the work, marks the\n"
    "   ticket done and starts the next approved one: run `ticket prompt` again and carry on.\n"
    "   Red saves the errors into the ticket: run `ticket prompt`, fix them, and run again.\n"
    "2. Never edit generated files (anything starting with `@generated`), the model, or\n"
    "   anything else in `.fsm/`, including the ticket. The gate detects it.\n"
    "3. Keep the trace tags at the top of each action and service file (`@action`,\n"
    "   `@service`, `@node`). Remove `// @stub` once the node's logic is real.\n"
    "4. `stategate gate` runs the same checks without committing. Report gate output as it\n"
    "   is; never claim something works when the gate says otherwise.\n"
    "5. Add a `## <ticket id>` entry to `CHANGELOG.md` that names every node ID in the ticket.\n"
    "6. After %d failed gate runs the ticket locks. Stop. Propose how to split it into\n"
    "   smaller steps; the architect decides.\n"
    "7. Never weaken, bypass or stub a locked item (safety invariants and locked actions).\n"
    "   If one is in your way, stop and ask.\n"
    "8. If the model looks wrong, propose a model change to the architect. Do not work\n"
    "   around it in code.\n";

static void append_if_missing(const char *path, const char *line) {
    char *text = read_file(path, NULL);
    if (text && strstr(text, line)) { free(text); return; }
    SB b;
    sb_init(&b);
    if (text) {
        sb_puts(&b, text);
        if (b.len && b.s[b.len - 1] != '\n') sb_puts(&b, "\n");
    }
    sb_printf(&b, "%s\n", line);
    write_file(path, b.s, b.len);
    free(text);
}

int cmd_init(const char *model_path) {
    mkdirs(TICKETS_DIR);
    if (!file_exists("AGENTS.md")) {
        SB b;
        sb_init(&b);
        sb_printf(&b, AGENTS_MD, model_path, MAX_FAILURES);
        write_file("AGENTS.md", b.s, b.len);
        printf("  wrote AGENTS.md\n");
    }
    if (!file_exists("CLAUDE.md")) {
        const char *c = "Read and follow AGENTS.md before doing anything in this project.\n";
        write_file("CLAUDE.md", c, strlen(c));
        printf("  wrote CLAUDE.md (points to AGENTS.md)\n");
    }
    if (!file_exists(CHANGELOG)) {
        const char *c = "# Changelog\n\nEach entry is headed by its ticket ID and names the node IDs it changed.\n";
        write_file(CHANGELOG, c, strlen(c));
        printf("  wrote %s\n", CHANGELOG);
    }
    append_if_missing(".gitignore", "/target");
    append_if_missing(".gitignore", "Cargo.lock");
    append_if_missing(".gitignore", GATE_JSON);

    int st;
    char *top = run_capture("git rev-parse --show-toplevel 2>/dev/null", &st);
    top[strcspn(top, "\n")] = 0;
    char cwd[4096];
    if (st == 0 && getcwd(cwd, sizeof cwd) && !strcmp(top, cwd)) {
        const char *hook = "#!/bin/sh\n# Installed by stategate init. The gate decides what lands.\nexec stategate gate\n";
        write_file(".git/hooks/pre-commit", hook, strlen(hook));
        chmod(".git/hooks/pre-commit", 0755);
        printf("  installed .git/hooks/pre-commit\n");
    } else {
        printf("  not a git repository root: pre-commit hook not installed\n");
    }
    printf("initialized. Next: stategate gen, then stategate signoff\n");
    return 0;
}

int cmd_export_mermaid(const char *model_path) {
    SgModel m;
    StrList e1 = {0}, e2 = {0}, e3 = {0};
    if (validate_all(model_path, &m, &e1, &e2, &e3) != 0) {
        print_errors("1 schema", &e1);
        print_errors("2 statechart", &e2);
        print_errors("3 invariants", &e3);
        return 1;
    }
    printf("stateDiagram-v2\n");
    for (int i = 0; i < m.n_states; i++)
        if (m.states[i].initial) printf("    [*] --> %s\n", m.states[i].name);
    for (int i = 0; i < m.n_states; i++) {
        const State *s = &m.states[i];
        for (int k = 0; k < s->n_on; k++) {
            const State *t = model_state(&m, s->on[k].target);
            printf("    %s --> %s : %s\n", s->name, t->name, s->on[k].event);
        }
    }
    for (int i = 0; i < m.n_invariants; i++) {
        const Invariant *v = &m.invariants[i];
        if (v->kind == INV_MAX_TIME_IN_STATE)
            printf("    note right of %s : %s max %ld ms\n", model_state(&m, v->state)->name, v->id, v->limit_ms);
    }
    return 0;
}
