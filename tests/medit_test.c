/* Tests for the canvas editor's YAML edits (src/medit.c): each edit must
 * keep the file's comments and style, change only what it says, and leave
 * a model that still validates.
 *
 * Usage: medit_test <examples/posture/posture.yaml> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "medit.h"
#include "model.h"
#include "validate.h"

static int failures;
static char tmp[] = "/tmp/medit_testXXXXXX";

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Loads and validates text; returns the number of errors (printed). */
static int validate_text(const char *what, const char *text, SgModel *m) {
    FILE *f = fopen(tmp, "wb");
    fputs(text, f);
    fclose(f);
    StrList e = {0};
    if (model_load(tmp, m, &e) == 0) {
        check_statechart(m, &e);
        if (!e.n) check_model_invariants(m, &e);
    }
    if (getenv("MEDIT_VERBOSE"))
        for (int i = 0; i < e.n; i++) printf("  %s: %s\n", what, e.v[i]);
    return e.n;
}

/* The validator reports state as neither handling nor ignoring ev. */
static int undecided(const char *text, const char *state, const char *ev) {
    FILE *f = fopen(tmp, "wb");
    fputs(text, f);
    fclose(f);
    SgModel m;
    StrList e = {0};
    char want[300];
    snprintf(want, sizeof want, "%s neither handles nor ignores %s ", state, ev);
    if (model_load(tmp, &m, &e) == 0) check_statechart(&m, &e);
    for (int i = 0; i < e.n; i++)
        if (strstr(e.v[i], want)) return 1;
    return 0;
}

/* Runs one edit (rc is evaluated first: it may move *text). */
static int ok_edit_(const char *what, int rc, const char *err, char **text, int want_valid) {
    SgModel m;
    CHECK(rc == 0, "%s: %s", what, err);
    int n = validate_text(what, *text, &m);
    if (want_valid) CHECK(n == 0, "%s: model no longer validates", what);
    return n;
}
#define ok_edit(what, call, err, t, want) do { int rc_ = (call); ok_edit_(what, rc_, err, &(t), want); } while (0)

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *s = calloc((size_t)n + 1, 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: medit_test posture.yaml\n"); return 2; }
    int fd = mkstemp(tmp);
    if (fd < 0) return 2;
    close(fd);
    char err[256] = "";
    SgModel m;

    /* A new model: one initial state looping on itself, and valid. */
    char *t = me_new_model("door");
    CHECK(validate_text("new", t, &m) == 0, "new model does not validate");
    CHECK(m.n_states == 1 && m.states[0].initial && m.states[0].n_on == 1 &&
          !strcmp(m.states[0].on[0].target, m.states[0].id), "new model is not one self-looping state");

    /* Draw it out: a second state, an arrow each way, then rename. Nothing
     * is ignored for the architect: undecided pairs stay validation errors
     * until they are drawn or ignored. */
    ok_edit("add Open", me_add_state(&t, "Open", 400, 120, err, sizeof err), err, t, 0);
    CHECK(strstr(t, "  - id: door.Open\n    name: Open\n    layout: { x: 400, y: 120 }\n"), "add Open:\n%s", t);
    CHECK(undecided(t, "door.Open", "TICK"), "Open should be undecided about TICK");
    ok_edit("Idle -PUSH-> Open", me_set_transition(&t, "door.Idle", "PUSH", "door.Open", err, sizeof err), err, t, 0);
    CHECK(strstr(t, "events: [TICK, PUSH]"), "PUSH not declared:\n%s", t);
    CHECK(strstr(t, "      TICK: door.Idle\n      PUSH: door.Open\n"), "PUSH not under on:\n%s", t);
    CHECK(!strstr(t, "ignore: ["), "something was ignored automatically:\n%s", t);
    CHECK(undecided(t, "door.Open", "PUSH"), "Open should be undecided about PUSH");
    CHECK(strstr(t, "# the black dot"), "comment lost");
    ok_edit("Open ignores TICK", me_ignore_event(&t, "door.Open", "TICK", err, sizeof err), err, t, 0);
    ok_edit("Open ignores PUSH", me_ignore_event(&t, "door.Open", "PUSH", err, sizeof err), err, t, 1);
    CHECK(strstr(t, "    name: Open\n    ignore: [TICK, PUSH]\n    layout:"), "ignore list:\n%s", t);
    CHECK(me_ignore_event(&t, "door.Idle", "PUSH", err, sizeof err) != 0, "ignored an event the state handles");
    CHECK(me_ignore_event(&t, "door.Idle", "NOPE", err, sizeof err) != 0, "ignored an undeclared event");
    ok_edit("Open -CLOSE-> Idle", me_set_transition(&t, "door.Open", "CLOSE", "door.Idle", err, sizeof err), err, t, 0);
    CHECK(strstr(t, "    on:\n      CLOSE: door.Idle\n    ignore: [TICK, PUSH]\n"), "on not before ignore:\n%s", t);
    CHECK(undecided(t, "door.Idle", "CLOSE"), "Idle should be undecided about CLOSE");
    ok_edit("Idle ignores CLOSE", me_ignore_event(&t, "door.Idle", "CLOSE", err, sizeof err), err, t, 1);
    ok_edit("rename Open", me_rename_state(&t, "door.Open", "Opened", err, sizeof err), err, t, 1);
    CHECK(strstr(t, "PUSH: door.Opened") && strstr(t, "name: Opened") && !strstr(t, "door.Open\n"), "rename:\n%s", t);
    CHECK(me_rename_state(&t, "door.Opened", "Idle", err, sizeof err) != 0, "rename onto an existing name");
    CHECK(me_add_state(&t, "lower", 0, 0, err, sizeof err) != 0, "a lower-case state name");
    CHECK(me_set_transition(&t, "door.Idle", "push", "door.Opened", err, sizeof err) != 0, "a lower-case event");

    /* Removing a transition leaves the pair undecided; an ignore elsewhere
     * keeps the event declared. */
    ok_edit("drop CLOSE", me_remove_transition(&t, "door.Opened", "CLOSE", err, sizeof err), err, t, 0);
    CHECK(undecided(t, "door.Opened", "CLOSE"), "Opened should be undecided about CLOSE");
    CHECK(strstr(t, "events: [TICK, PUSH, CLOSE]"), "CLOSE is still ignored by Idle, so it stays:\n%s", t);
    ok_edit("add SHUT", me_set_transition(&t, "door.Opened", "SHUT", "door.Idle", err, sizeof err), err, t, 0);
    ok_edit("drop SHUT", me_remove_transition(&t, "door.Opened", "SHUT", err, sizeof err), err, t, 0);
    CHECK(!strstr(t, "SHUT"), "SHUT is used nowhere, so it leaves:\n%s", t);
    ok_edit("Opened -CLOSE-> Idle", me_set_transition(&t, "door.Opened", "CLOSE", "door.Idle", err, sizeof err), err, t, 1);
    ok_edit("move", me_set_layout(&t, "door.Opened", 700, 300, err, sizeof err), err, t, 1);
    CHECK(strstr(t, "layout: { x: 700, y: 300 }"), "move:\n%s", t);
    ok_edit("initial", me_set_initial(&t, "door.Opened", err, sizeof err), err, t, 1);
    CHECK(validate_text("initial", t, &m) == 0 && model_state(&m, "door.Opened")->initial && !model_state(&m, "door.Idle")->initial, "initial moved:\n%s", t);
    ok_edit("delete Idle", me_delete_state(&t, "door.Idle", err, sizeof err), err, t, 0);
    /* Opened's CLOSE went to Idle, and only Idle ignored it: it leaves. */
    CHECK(!strstr(t, "CLOSE"), "CLOSE left behind:\n%s", t);
    CHECK(validate_text("delete", t, &m) >= 0 && m.n_states == 1 && m.states[0].initial, "after delete:\n%s", t);
    CHECK(me_delete_state(&t, "door.Opened", err, sizeof err) != 0, "deleted the last state");
    free(t);

    /* The posture example: comments and flow lists survive, each edit is local. */
    char *orig = slurp(argv[1]);
    t = strdup(orig);
    ok_edit("posture move", me_set_layout(&t, "posture.Calibrating", 360, 40, err, sizeof err), err, t, 1);
    {
        /* Only the one number changed. */
        size_t a = 0;
        while (orig[a] && orig[a] == t[a]) a++;
        CHECK(!strncmp(t + a, "60, y: 40 }", 11) && strlen(t) == strlen(orig), "posture move changed more than x");
    }
    ok_edit("posture new event", me_set_transition(&t, "posture.Monitoring", "PAUSE", "posture.Off", err, sizeof err), err, t, 0);
    CHECK(strstr(t, "      FAULT: posture.Off\n      PAUSE: posture.Off\n    ignore: [POWER_ON, CALIBRATED, UPRIGHT"), "Monitoring PAUSE:\n%s", t);
    CHECK(undecided(t, "posture.Off", "PAUSE") && undecided(t, "posture.Nudging", "PAUSE"), "others undecided about PAUSE");
    CHECK(strstr(t, "# grace period before nudging") && strstr(t, "# every way out stops stimulation"), "comments lost");
    ok_edit("posture remove PAUSE", me_remove_transition(&t, "posture.Monitoring", "PAUSE", err, sizeof err), err, t, 1);
    {
        /* Back to the original apart from the move. */
        char *u = strdup(orig);
        me_set_layout(&u, "posture.Calibrating", 360, 40, err, sizeof err);
        CHECK(!strcmp(t, u), "add+remove PAUSE is not a no-op:\n%s", t);
        free(u);
    }
    ok_edit("posture rename", me_rename_state(&t, "posture.Off", "Sleep", err, sizeof err), err, t, 1);
    CHECK(strstr(t, "target: posture.Sleep") && strstr(t, "FAULT: posture.Sleep") && !strstr(t, "posture.Off"), "rename refs:\n%s", t);
    ok_edit("posture delete Calibrating", me_delete_state(&t, "posture.Calibrating", err, sizeof err), err, t, 0);
    CHECK(validate_text("posture delete", t, &m) > 0 && m.n_states == 4, "4 states left");
    /* Off's POWER_ON went to Calibrating: Off is now undecided about it;
     * the other states still ignore POWER_ON and CALIBRATED, so both stay. */
    CHECK(undecided(t, "posture.Sleep", "POWER_ON"), "Sleep undecided about POWER_ON:\n%s", t);
    CHECK(strstr(t, "    entry: [stim_off]\n    on:\n      FAULT: posture.Sleep\n    ignore: [POWER_OFF, CALIBRATED"), "Off's ignore:\n%s", t);
    ok_edit("posture add state", me_add_state(&t, "Charging", 40, 460, err, sizeof err), err, t, 0);
    CHECK(strstr(t, "    layout: { x: 340, y: 460 }\n\n  - id: posture.Charging\n    name: Charging\n"), "blank line before the new state:\n%s", t);
    free(t);
    free(orig);

    /* Flow-style states and a block-style events list. */
    t = strdup("machine: { id: m, version: 1, language: rust }\nevents:\n  - A\nstates:\n  - { id: m.One, initial: true, on: { A: m.One } }\n");
    ok_edit("flow add", me_add_state(&t, "Two", 300, 0, err, sizeof err), err, t, 0);
    ok_edit("flow arrow", me_set_transition(&t, "m.One", "B", "m.Two", err, sizeof err), err, t, 0);
    ok_edit("flow back", me_set_transition(&t, "m.Two", "A", "m.One", err, sizeof err), err, t, 0);
    ok_edit("flow ignore", me_ignore_event(&t, "m.Two", "B", err, sizeof err), err, t, 1);
    CHECK(strstr(t, "events:\n  - A\n  - B\n") && strstr(t, "{ id: m.One, initial: true, on: { A: m.One, B: m.Two } }"), "flow:\n%s", t);
    CHECK(strstr(t, "ignore: [B]"), "flow ignore:\n%s", t);
    ok_edit("flow move", me_set_layout(&t, "m.One", 10, 20, err, sizeof err), err, t, 1);
    ok_edit("flow delete", me_delete_state(&t, "m.One", err, sizeof err), err, t, 0);
    free(t);

    unlink(tmp);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("medit: all checks passed\n");
    return 0;
}
