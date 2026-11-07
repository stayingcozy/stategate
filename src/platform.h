#ifndef SG_PLATFORM_H
#define SG_PLATFORM_H

/* What differs between POSIX (Linux, macOS) and Windows (MinGW-w64): running
 * other programs, paths, the file system and the terminal. Everything else
 * in stategate is plain C. Paths this layer returns always use '/', so the
 * rest of the code splits and joins them the same way on every system.
 * Windows headers stay inside platform.c: raylib's names clash with them. */

#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#define SG_EXE_SUFFIX ".exe"
#else
#define SG_EXE_SUFFIX ""
#endif

/* Call first in main(). On Windows: stdout unbuffered (a pipe is otherwise
 * fully buffered, and the app shows output as it comes), and the app's
 * confirmation hand-off read and cleared (see sg_stdin_is_person). */
void sg_platform_init(void);

/* Runs argv[0] (looked up on PATH) with argv, no shell, stdin empty.
 * Returns stdout, with stderr appended when merge_stderr is set and
 * discarded otherwise. *status gets the exit code: 127 when the program
 * cannot be started, -1 when it did not exit normally. On Windows, when
 * argv[0] is cmd.exe its arguments are passed as written, unquoted. */
char *run_argv(const char *const argv[], int merge_stderr, int *status);

/* Runs argv with this process's stdin, stdout and stderr. POSIX: becomes
 * the program and returns only when it cannot start (127). Windows: waits
 * and returns its exit code. */
int run_exec(const char *const argv[]);

/* Finds a program on PATH (on Windows also with .exe, .cmd, .bat). Returns
 * 1 and the full path in out when found. */
int sg_which(const char *name, char *out, size_t n);

/* This binary's own path. */
void sg_self_exe(char *out, size_t n, const char *argv0);

/* realpath(3): absolute, symlinks resolved, NULL when the path does not
 * exist. out holds at least SG_PATH_MAX bytes. */
#define SG_PATH_MAX 4096
char *sg_realpath(const char *path, char *out);
/* getcwd(3) with '/' separators. */
char *sg_getcwd(char *out, size_t n);
/* 1 when a and b name the same path (case-insensitive on Windows). */
int sg_path_eq(const char *a, const char *b);
/* 1 when path is dir or inside it. */
int sg_path_within(const char *path, const char *dir);

int sg_mkdir(const char *path);
int sg_is_dir(const char *path);
int sg_is_symlink(const char *path);
/* Changes with every write, even two in one second. 0 when missing. */
long long sg_file_sig(const char *path);

/* The person's home folder, or NULL. */
const char *sg_home(void);
/* <config dir>/stategate/<name>: $XDG_CONFIG_HOME or ~/.config on POSIX,
 * %APPDATA% on Windows. */
void sg_config_path(char *out, size_t n, const char *name);

/* Puts dir first on PATH for this process and what it starts. */
void sg_path_prepend(const char *dir);
/* Opens a file in the person's default app; does not wait. */
void sg_open_default_app(const char *path);

/* 1 when a person can answer on stdin: an interactive terminal, or on
 * Windows the app's confirmation box (Windows has no pty to hand the CLI,
 * so the app says so in STATEGATE_APP_CONFIRM, which sg_platform_init
 * clears so nothing this process starts inherits it). */
int sg_stdin_is_person(void);
/* No "\n" -> "\r\n" translation on stdin and stdout (the MCP server). */
void sg_binary_stdio(void);

/* fnmatch(pattern, path, FNM_PATHNAME): '*', '?' and [...] never match '/',
 * and '\' escapes; [:class:] works, collating symbols ([.x.], [=x=]) do
 * not. Returns 0 on a match. The same code on every system, so
 * the gate decides the same way everywhere. */
int sg_fnmatch(const char *pattern, const char *path);

#ifdef _WIN32
/* For the app's runner: starts argv with stdout and stderr on a pipe, and
 * stdin on a pipe too when interactive (then the CLI takes the app's
 * confirmations). Handles are HANDLEs. Returns 0 on start. */
int sg_spawn_job(const char *const argv[], int interactive, void **proc, void **out_rd, void **in_wr);
#endif

/* getline(3). Returns the length, or -1 at end of file. */
long sg_getline(char **line, size_t *cap, FILE *f);

#endif
