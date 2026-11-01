#include "runner.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

void job_clear(Job *j) {
    free(j->out.s);
    memset(j, 0, sizeof *j);
    j->fd = -1;
}

int job_start(Job *j, const char *title, char *const argv[], int use_pty) {
    if (j->running) return -1;
    free(j->out.s);
    memset(j, 0, sizeof *j);
    sb_init(&j->out);
    snprintf(j->title, sizeof j->title, "%s", title);
    j->pty = use_pty;
    pid_t pid;
    int fd;
    if (use_pty) {
        pid = forkpty(&fd, NULL, NULL, NULL);
        if (pid == 0) {
            execvp(argv[0], argv);
            fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
    } else {
        int p[2];
        if (pipe(p) != 0) return -1;
        pid = fork();
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) dup2(devnull, STDIN_FILENO);
            dup2(p[1], STDOUT_FILENO);
            dup2(p[1], STDERR_FILENO);
            close(p[0]);
            close(p[1]);
            setsid(); /* no controlling terminal: confirmations are refused */
            execvp(argv[0], argv);
            fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
        close(p[1]);
        fd = p[0];
    }
    if (pid < 0) return -1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    j->pid = pid;
    j->fd = fd;
    j->running = 1;
    return 0;
}

int job_poll(Job *j) {
    if (!j->running) return 0;
    int changed = 0;
    char buf[4096];
    for (;;) {
        ssize_t n = read(j->fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] != '\r') sb_printf(&j->out, "%c", buf[i]);
            changed = 1;
            continue;
        }
        break; /* EAGAIN, EOF, or EIO when a pty child has exited */
    }
    int st;
    if (waitpid(j->pid, &st, WNOHANG) == j->pid) {
        /* Drain what is left, then close. */
        ssize_t n;
        while ((n = read(j->fd, buf, sizeof buf)) > 0)
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] != '\r') sb_printf(&j->out, "%c", buf[i]);
        close(j->fd);
        j->fd = -1;
        j->running = 0;
        j->exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        changed = 1;
    }
    return changed;
}

void job_send(Job *j, const char *line) {
    if (!j->running || !j->pty) return;
    size_t n = strlen(line);
    if (write(j->fd, line, n) != (ssize_t)n || write(j->fd, "\n", 1) != 1) { /* the job ended */ }
}

void job_stop(Job *j) {
    if (!j->running) return;
    kill(j->pid, SIGTERM);
}
