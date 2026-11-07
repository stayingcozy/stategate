#include "platform.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif


#ifdef _WIN32
static void to_slashes(char *p);
#endif

/* ---- the same everywhere ---- */

long sg_getline(char **line, size_t *cap, FILE *f) {
    if (!*line || *cap < 2) {
        *cap = 256;
        *line = xrealloc(*line, *cap);
    }
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 2 > *cap) {
            *cap *= 2;
            *line = xrealloc(*line, *cap);
        }
        (*line)[n++] = (char)c;
        if (c == '\n') break;
    }
    (*line)[n] = 0;
    return n ? (long)n : -1;
}

int sg_path_within(const char *path, const char *dir) {
    size_t n = strlen(dir);
    if (n && dir[n - 1] == '/') n--; /* "/" or "C:/" */
#ifdef _WIN32
    if (_strnicmp(path, dir, n)) return 0;
#else
    if (strncmp(path, dir, n)) return 0;
#endif
    return path[n] == '/' || path[n] == 0;
}

int sg_path_eq(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

int sg_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void sg_config_path(char *out, size_t n, const char *name) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        snprintf(out, n, "%s/stategate/%s", appdata, name);
        to_slashes(out);
        return;
    }
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(out, n, "%s/stategate/%s", xdg, name);
        return;
    }
#endif
    const char *home = sg_home();
    snprintf(out, n, "%s/.config/stategate/%s", home ? home : ".", name);
}

/* Character classes inside [...]: [:alpha:] and friends. */
static int class_match(const char *name, size_t len, unsigned char c) {
    static const struct { const char *n; int (*f)(int); } classes[] = {
        {"alnum", isalnum}, {"alpha", isalpha}, {"blank", isblank}, {"cntrl", iscntrl},
        {"digit", isdigit}, {"graph", isgraph}, {"lower", islower}, {"print", isprint},
        {"punct", ispunct}, {"space", isspace}, {"upper", isupper}, {"xdigit", isxdigit}};
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (strlen(classes[i].n) == len && !strncmp(classes[i].n, name, len)) return classes[i].f(c) != 0;
    return 0;
}

/* Matches one [...] against c. Returns the pattern after the ']', or NULL
 * when the bracket is not closed (then '[' is an ordinary character). */
static const char *bracket(const char *p, unsigned char c, int *matched) {
    int neg = *p == '!' || *p == '^';
    if (neg) p++;
    int hit = 0, first = 1;
    for (;;) {
        if (!*p) return NULL;
        if (*p == ']' && !first) break;
        first = 0;
        if (p[0] == '[' && p[1] == ':') {
            const char *end = p + 2;
            while (islower((unsigned char)*end)) end++;
            if (end[0] == ':' && end[1] == ']') {
                hit |= class_match(p + 2, (size_t)(end - p - 2), c);
                p = end + 2;
                continue;
            } /* else '[' is an ordinary character */
        }
        unsigned char lo = (unsigned char)*p++;
        if (lo == '\\' && *p) lo = (unsigned char)*p++;
        unsigned char hi = lo;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            p++;
            hi = (unsigned char)*p++;
            if (hi == '\\' && *p) hi = (unsigned char)*p++;
        }
        if (lo <= c && c <= hi) hit = 1;
    }
    *matched = hit != neg;
    return p + 1;
}

int sg_fnmatch(const char *p, const char *s) {
    for (;;) {
        char c = *p++;
        switch (c) {
        case 0:
            return *s ? 1 : 0;
        case '?':
            if (!*s || *s == '/') return 1;
            s++;
            break;
        case '*':
            while (*p == '*') p++;
            if (!*p) return strchr(s, '/') ? 1 : 0;
            for (const char *t = s;; t++) {
                if (sg_fnmatch(p, t) == 0) return 0;
                if (!*t || *t == '/') return 1;
            }
        case '[': {
            int matched = 0;
            const char *rest = *s && *s != '/' ? bracket(p, (unsigned char)*s, &matched) : NULL;
            if (rest) {
                if (!matched) return 1;
                p = rest;
                s++;
                break;
            }
            /* No closing ']', or nothing to match here: compare '[' as is. */
            if (*s != '[') return 1;
            s++;
            break;
        }
        case '\\':
            if (!*p) return 1; /* a pattern may not end in a backslash */
            c = *p++;
            /* fall through */
        default:
            if (*s != c) return 1;
            s++;
        }
    }
}

#ifdef _WIN32

/* ---- Windows ---- */

static void to_slashes(char *p) {
    for (; *p; p++) if (*p == '\\') *p = '/';
}

static int g_app_confirm;

void sg_platform_init(void) {
    const char *v = getenv("STATEGATE_APP_CONFIRM");
    g_app_confirm = v && !strcmp(v, "1");
    _putenv("STATEGATE_APP_CONFIRM=");
    setvbuf(stdout, NULL, _IONBF, 0);
}

int sg_stdin_is_person(void) {
    DWORD mode;
    return g_app_confirm || GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
}

void sg_binary_stdio(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

static int is_file(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* name in dir, as is when it has an extension, then with each one. */
static int try_exts(const char *dir, const char *name, char *out, size_t n) {
    static const char *const exts[] = {".exe", ".cmd", ".bat", ".com"};
    char base[MAX_PATH * 2];
    if (dir) snprintf(base, sizeof base, "%s\\%s", dir, name);
    else snprintf(base, sizeof base, "%s", name);
    const char *dot = strrchr(name, '.');
    if (dot && !strpbrk(dot, "/\\") && is_file(base)) { snprintf(out, n, "%s", base); return 1; }
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++) {
        snprintf(out, n, "%s%s", base, exts[i]);
        if (is_file(out)) return 1;
    }
    return 0;
}

int sg_which(const char *name, char *out, size_t n) {
    if (!name || !*name) return 0;
    if (strpbrk(name, "/\\:")) return try_exts(NULL, name, out, n);
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = xstrdup(path), *save = NULL;
    int found = 0;
    for (char *d = strtok_r(copy, ";", &save); d && !found; d = strtok_r(NULL, ";", &save)) {
        if (*d == '"') {
            d++;
            char *q = strchr(d, '"');
            if (q) *q = 0;
        }
        if (*d) found = try_exts(d, name, out, n);
    }
    free(copy);
    return found;
}

/* One argument as the C runtime's command-line parser reads it back. */
static void quote_arg(SB *b, const char *a) {
    if (*a && !strpbrk(a, " \t\n\v\"")) { sb_puts(b, a); return; }
    sb_puts(b, "\"");
    for (const char *c = a;; c++) {
        size_t bs = 0;
        while (*c == '\\') { bs++; c++; }
        if (!*c) {
            for (size_t i = 0; i < bs * 2; i++) sb_puts(b, "\\");
            break;
        }
        for (size_t i = 0; i < (*c == '"' ? bs * 2 + 1 : bs); i++) sb_puts(b, "\\");
        char ch[2] = {*c, 0};
        sb_puts(b, ch);
    }
    sb_puts(b, "\"");
}

/* The command line for argv. A .cmd or .bat (e.g. an npm-installed tool)
 * runs through cmd.exe; *app gets the program to start, or NULL to let
 * CreateProcess search for it. cmd.exe reads its own command line, not
 * C-runtime quoting, so arguments to cmd.exe itself go in as they are. */
static char *command_line(const char *const argv[], char app[MAX_PATH * 2]) {
    SB b;
    sb_init(&b);
    if (!_stricmp(argv[0], "cmd.exe") || !_stricmp(argv[0], "cmd")) {
        const char *comspec = getenv("ComSpec");
        snprintf(app, MAX_PATH * 2, "%s", comspec && *comspec ? comspec : "C:\\Windows\\System32\\cmd.exe");
        sb_puts(&b, "cmd.exe");
        for (int i = 1; argv[i]; i++) {
            sb_puts(&b, " ");
            sb_puts(&b, argv[i]);
        }
        return b.s;
    }
    char found[MAX_PATH * 2];
    int have = sg_which(argv[0], found, sizeof found);
    const char *ext = have ? strrchr(found, '.') : NULL;
    int batch = ext && (!_stricmp(ext, ".cmd") || !_stricmp(ext, ".bat"));
    if (batch) {
        const char *comspec = getenv("ComSpec");
        snprintf(app, MAX_PATH * 2, "%s", comspec && *comspec ? comspec : "C:\\Windows\\System32\\cmd.exe");
        sb_puts(&b, "cmd.exe /d /s /c \"");
    } else {
        if (have) snprintf(app, MAX_PATH * 2, "%s", found);
        else app[0] = 0;
    }
    quote_arg(&b, have ? found : argv[0]);
    for (int i = 1; argv[i]; i++) {
        sb_puts(&b, " ");
        quote_arg(&b, argv[i]);
    }
    if (batch) sb_puts(&b, "\"");
    return b.s;
}

/* Starts argv. With std handles given, the child gets those (they must be
 * inheritable); otherwise it shares this console. */
static int spawn(const char *const argv[], HANDLE in, HANDLE out, HANDLE err, int app_confirm,
                 PROCESS_INFORMATION *pi) {
    char app[MAX_PATH * 2];
    char *cmd = command_line(argv, app);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (in || out || err) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in;
        si.hStdOutput = out;
        si.hStdError = err;
    }
    if (app_confirm) SetEnvironmentVariableA("STATEGATE_APP_CONFIRM", "1");
    BOOL ok = CreateProcessA(app[0] ? app : NULL, cmd, NULL, NULL, TRUE,
                             in || out || err ? CREATE_NO_WINDOW : 0, NULL, NULL, &si, pi);
    if (app_confirm) SetEnvironmentVariableA("STATEGATE_APP_CONFIRM", NULL);
    free(cmd);
    return ok ? 0 : -1;
}

static HANDLE open_nul(int write) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    return CreateFileA("NUL", write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                       OPEN_EXISTING, 0, NULL);
}

char *run_argv(const char *const argv[], int merge_stderr, int *status) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { *status = -1; return xstrdup("pipe failed"); }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE in = open_nul(0), err = merge_stderr ? wr : open_nul(1);
    PROCESS_INFORMATION pi;
    int r = spawn(argv, in, wr, err, 0, &pi);
    CloseHandle(wr);
    CloseHandle(in);
    if (!merge_stderr) CloseHandle(err);
    SB b;
    sb_init(&b);
    if (r != 0) {
        CloseHandle(rd);
        *status = 127;
        return b.s;
    }
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf - 1, &n, NULL) && n > 0) {
        buf[n] = 0;
        sb_puts(&b, buf);
    }
    /* A text-mode child writes "\r\n": hand back plain "\n" lines. */
    size_t j = 0;
    for (size_t i = 0; i < b.len; i++)
        if (!(b.s[i] == '\r' && b.s[i + 1] == '\n')) b.s[j++] = b.s[i];
    b.s[j] = 0;
    b.len = j;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    *status = (int)code;
    return b.s;
}

int run_exec(const char *const argv[]) {
    PROCESS_INFORMATION pi;
    fflush(stdout);
    fflush(stderr);
    if (spawn(argv, NULL, NULL, NULL, 0, &pi) != 0) {
        fprintf(stderr, "%s: cannot run (error %lu)\n", argv[0], (unsigned long)GetLastError());
        return 127;
    }
    SetConsoleCtrlHandler(NULL, TRUE); /* Ctrl+C reaches the child; we wait */
    WaitForSingleObject(pi.hProcess, INFINITE);
    SetConsoleCtrlHandler(NULL, FALSE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int sg_spawn_job(const char *const argv[], int interactive, void **proc, void **out_rd, void **in_wr) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr, in_r = NULL, in_w = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    if (interactive) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) { CloseHandle(rd); CloseHandle(wr); return -1; }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        in_r = open_nul(0);
    }
    PROCESS_INFORMATION pi;
    int r = spawn(argv, in_r, wr, wr, interactive, &pi);
    CloseHandle(wr);
    CloseHandle(in_r);
    if (r != 0) {
        CloseHandle(rd);
        if (in_w) CloseHandle(in_w);
        return -1;
    }
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    *out_rd = rd;
    *in_wr = in_w;
    return 0;
}

void sg_self_exe(char *out, size_t n, const char *argv0) {
    DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (len > 0 && len < n) { to_slashes(out); return; }
    if (!sg_which(argv0, out, n)) snprintf(out, n, "%s", argv0);
    to_slashes(out);
}

char *sg_realpath(const char *path, char *out) {
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = ENOENT; return NULL; }
    char buf[SG_PATH_MAX];
    DWORD n = GetFinalPathNameByHandleA(h, buf, sizeof buf, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= sizeof buf) { errno = ENAMETOOLONG; return NULL; }
    const char *p = buf;
    if (!strncmp(p, "\\\\?\\UNC\\", 8)) {
        snprintf(out, SG_PATH_MAX, "\\\\%s", p + 8);
    } else {
        if (!strncmp(p, "\\\\?\\", 4)) p += 4;
        snprintf(out, SG_PATH_MAX, "%s", p);
    }
    to_slashes(out);
    return out;
}

char *sg_getcwd(char *out, size_t n) {
    if (!_getcwd(out, (int)n)) return NULL;
    to_slashes(out);
    return out;
}

int sg_mkdir(const char *path) { return _mkdir(path); }

int sg_is_symlink(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

long long sg_file_sig(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    long long t = (long long)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;
    long long size = (long long)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    return t + size * 7;
}

const char *sg_home(void) {
    static char home[SG_PATH_MAX];
    const char *h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    if (!h || !*h) return NULL;
    snprintf(home, sizeof home, "%s", h);
    to_slashes(home);
    return home;
}

void sg_path_prepend(const char *dir) {
    const char *path = getenv("PATH");
    size_t n = strlen(dir) + (path ? strlen(path) : 0) + 2;
    char *np = xcalloc(n, 1);
    snprintf(np, n, "%s;%s", dir, path ? path : "");
    for (char *c = np; *c && *c != ';'; c++) if (*c == '/') *c = '\\';
    _putenv_s("PATH", np);
    free(np);
}

void sg_open_default_app(const char *path) {
    char p[SG_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p; *c; c++) if (*c == '/') *c = '\\';
    ShellExecuteA(NULL, "open", p, NULL, NULL, SW_SHOWNORMAL);
}

#else

/* ---- POSIX ---- */

void sg_platform_init(void) {}

int sg_stdin_is_person(void) { return isatty(STDIN_FILENO); }

void sg_binary_stdio(void) {}

int sg_which(const char *name, char *out, size_t n) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) {
        snprintf(out, n, "%s", name);
        return access(out, X_OK) == 0 && !sg_is_dir(out);
    }
    const char *path = getenv("PATH");
    if (!path) return 0;
    for (const char *d = path;;) {
        const char *end = strchr(d, ':');
        size_t len = end ? (size_t)(end - d) : strlen(d);
        snprintf(out, n, "%.*s/%s", (int)(len ? len : 1), len ? d : ".", name);
        if (access(out, X_OK) == 0 && !sg_is_dir(out)) return 1;
        if (!end) return 0;
        d = end + 1;
    }
}

char *run_argv(const char *const argv[], int merge_stderr, int *status) {
    int fd[2];
    if (pipe(fd) != 0) { *status = -1; return xstrdup("pipe failed"); }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        *status = -1;
        return xstrdup("fork failed");
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(fd[1], STDOUT_FILENO);
        if (merge_stderr) dup2(fd[1], STDERR_FILENO);
        else if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(fd[0]);
        close(fd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fd[1]);
    SB b;
    sb_init(&b);
    char buf[4096];
    ssize_t r;
    while ((r = read(fd[0], buf, sizeof buf - 1)) > 0 || (r < 0 && errno == EINTR)) {
        if (r <= 0) continue;
        buf[r] = 0;
        sb_puts(&b, buf);
    }
    close(fd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return b.s;
}

int run_exec(const char *const argv[]) {
    fflush(stdout);
    fflush(stderr);
    execvp(argv[0], (char *const *)argv);
    perror(argv[0]);
    return 127;
}

void sg_self_exe(char *out, size_t n, const char *argv0) {
#if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", out, n - 1);
    if (len > 0) { out[len] = 0; return; }
#elif defined(__APPLE__)
    char buf[PATH_MAX], resolved[SG_PATH_MAX];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0 && realpath(buf, resolved)) { snprintf(out, n, "%s", resolved); return; }
#endif
    char real[SG_PATH_MAX];
    if (strchr(argv0, '/') && realpath(argv0, real)) snprintf(out, n, "%s", real);
    else if (!sg_which(argv0, out, n)) snprintf(out, n, "%s", argv0);
}

char *sg_realpath(const char *path, char *out) { return realpath(path, out); }

char *sg_getcwd(char *out, size_t n) { return getcwd(out, n); }

int sg_mkdir(const char *path) { return mkdir(path, 0755); }

int sg_is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

long long sg_file_sig(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#ifdef __APPLE__
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec + (long long)st.st_size * 7;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec + (long long)st.st_size * 7;
#endif
}

const char *sg_home(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : NULL;
}

void sg_path_prepend(const char *dir) {
    const char *path = getenv("PATH");
    size_t n = strlen(dir) + (path ? strlen(path) : 0) + 2;
    char *np = xcalloc(n, 1);
    snprintf(np, n, "%s:%s", dir, path ? path : "");
    setenv("PATH", np, 1);
    free(np);
}

void sg_open_default_app(const char *path) {
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid == 0) {
        /* A grandchild, so the app never waits on the editor. */
        if (fork() == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            setsid();
            execlp(opener, opener, path, (char *)NULL);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

#endif
