#ifndef SG_RUNNER_H
#define SG_RUNNER_H

#include <sys/types.h>

#include "util.h"

/* One command the app runs without blocking the window. With a pty the
 * command sees a terminal, so the CLI's "a person must confirm" prompts
 * (sign-off, ticket review) are answered by the person clicking in the app. */
typedef struct {
    char title[256];
    pid_t pid;
    int fd;
    int running;
    int exit_code; /* valid once running is 0 */
    int pty;
    SB out;        /* stdout and stderr together, '\r' removed */
} Job;

/* argv[0] is looked up on PATH. Returns 0 on start. */
int job_start(Job *j, const char *title, char *const argv[], int use_pty);
/* Reads what is available and reaps the process when it ends. Returns 1
 * when the output grew or the job ended this call. */
int job_poll(Job *j);
/* Types a line into a pty job. */
void job_send(Job *j, const char *line);
void job_stop(Job *j);
void job_clear(Job *j);

#endif
