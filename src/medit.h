#ifndef SG_MEDIT_H
#define SG_MEDIT_H

#include <stddef.h>

/* Edits to a model's YAML text, for the app's canvas editor.
 *
 * The YAML stays the source of truth: each edit parses the text, finds the
 * nodes it changes and splices just those bytes, so comments, key order and
 * flow or block style are kept and a diff shows only the change.
 *
 * Every function takes the text by reference (a malloc'd, NUL-terminated
 * buffer it may reallocate) and returns 0, or -1 with a sentence in err.
 * On an error the text may be partly edited: callers keep their own copy.
 *
 * The edits keep the model total: a new event is ignored by every other
 * state, a removed transition's event becomes ignored by its state, and an
 * event nothing uses any more leaves the events list. */

/* A state named `name` (CamelCase), id <machine>.<name>, at layout (x, y).
 * It ignores every declared event until transitions are drawn from it. */
int me_add_state(char **text, const char *name, double x, double y, char *err, size_t errn);
/* Renames a state: its id, its name and every reference to the id. */
int me_rename_state(char **text, const char *id, const char *new_name, char *err, size_t errn);
/* Deletes a state and the transitions into it (their events become
 * ignored). Deleting the initial state makes the first state left initial. */
int me_delete_state(char **text, const char *id, char *err, size_t errn);
int me_set_layout(char **text, const char *id, double x, double y, char *err, size_t errn);
int me_set_initial(char **text, const char *id, char *err, size_t errn);
/* from --event--> to: adds the transition, or retargets it if `from`
 * already handles the event. A new event is declared under `events`. */
int me_set_transition(char **text, const char *from, const char *event, const char *to, char *err, size_t errn);
int me_remove_transition(char **text, const char *from, const char *event, char *err, size_t errn);

/* A new model: one initial state that loops back on itself. malloc'd. */
char *me_new_model(const char *machine_id);

int me_is_event_name(const char *s);  /* UPPER_SNAKE_CASE */
int me_is_state_name(const char *s);  /* CamelCase letters and digits */

#endif
