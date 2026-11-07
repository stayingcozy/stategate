#include "runner.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include "platform.h"

void job_clear(Job *j) {
    free(j->out.s);
    memset(j, 0, sizeof *j);
}

int job_start(Job *j, const char *title, const char *const argv[], int use_pty) {
    if (j->running) return -1;
    free(j->out.s);
    memset(j, 0, sizeof *j);
    sb_init(&j->out);
    snprintf(j->title, sizeof j->title, "%s", title);
    j->pty = use_pty;
    if (sg_spawn_job(argv, use_pty, &j->proc, &j->rd, &j->wr) != 0) return -1;
    j->running = 1;
    return 0;
}

/* Reads what the pipe holds now, without waiting. */
static int drain(Job *j) {
    int changed = 0;
    char buf[4096];
    DWORD avail, n;
    while (PeekNamedPipe(j->rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        if (!ReadFile(j->rd, buf, avail < sizeof buf ? avail : sizeof buf, &n, NULL) || n == 0) break;
        for (DWORD i = 0; i < n; i++)
            if (buf[i] != '\r') sb_printf(&j->out, "%c", buf[i]);
        changed = 1;
    }
    return changed;
}

int job_poll(Job *j) {
    if (!j->running) return 0;
    int changed = drain(j);
    if (WaitForSingleObject(j->proc, 0) == WAIT_OBJECT_0) {
        changed |= drain(j);
        DWORD code = 1;
        GetExitCodeProcess(j->proc, &code);
        CloseHandle(j->proc);
        CloseHandle(j->rd);
        if (j->wr) CloseHandle(j->wr);
        j->proc = j->rd = j->wr = NULL;
        j->running = 0;
        j->exit_code = (int)code;
        changed = 1;
    }
    return changed;
}

void job_send(Job *j, const char *line) {
    if (!j->running || !j->pty || !j->wr) return;
    DWORD n;
    WriteFile(j->wr, line, (DWORD)strlen(line), &n, NULL);
    WriteFile(j->wr, "\n", 1, &n, NULL);
}

void job_stop(Job *j) {
    if (!j->running) return;
    TerminateProcess(j->proc, 1);
}

#else

#include <fcntl.h>
#include <signal.h>
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

int job_start(Job *j, const char *title, const char *const argv[], int use_pty) {
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
            execvp(argv[0], (char *const *)argv);
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
            execvp(argv[0], (char *const *)argv);
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

#endif
