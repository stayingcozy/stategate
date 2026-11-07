#ifndef SG_RUNNER_H
#define SG_RUNNER_H

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "util.h"

/* One command the app runs without blocking the window. With a pty the
 * command sees a terminal, so the CLI's "a person must confirm" prompts
 * (sign-off, ticket review) are answered by the person clicking in the app.
 * On Windows, where there is no pty, stdin is a pipe from the app and the
 * CLI is told so (STATEGATE_APP_CONFIRM, see src/platform.h). */
typedef struct {
    char title[256];
#ifdef _WIN32
    void *proc, *rd, *wr; /* HANDLEs: its output, and its stdin when pty is set */
#else
    pid_t pid;
    int fd;
#endif
    int running;
    int exit_code; /* valid once running is 0 */
    int pty;
    SB out;        /* stdout and stderr together, '\r' removed */
} Job;

/* argv[0] is looked up on PATH. Returns 0 on start. */
int job_start(Job *j, const char *title, const char *const argv[], int use_pty);
/* Reads what is available and reaps the process when it ends. Returns 1
 * when the output grew or the job ended this call. */
int job_poll(Job *j);
/* Types a line into a pty job. */
void job_send(Job *j, const char *line);
void job_stop(Job *j);
void job_clear(Job *j);

#endif
