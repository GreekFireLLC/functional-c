/*
 * Pure C17 acceptance-test harness for fc2c (zero .NET, zero Python, zero third-party packages).
 *
 * Pipeline for behavioral tests:
 *   .fc file -> fc2c -> generated .c -> gcc -std=c17 -Wall -Wextra -Werror -> run native binary
 *
 * Rejection tests verify:
 *   exit code 1, no output file written, no .tmp files left behind, and exact diagnostic
 *   file(line,col): error FCnnnn: <message substring>.
 *
 * Usage:
 *   test_runner [--report path.md] [--filter text] [--fc2c path] [--cc gcc]
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------- dynamic string buffer */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = 0; b->cap = 0; }

static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + extra + 1) nc *= 2;
    char *nd = realloc(b->data, nc);
    if (!nd) { fprintf(stderr, "out of memory\n"); exit(2); }
    b->data = nd;
    b->cap = nc;
}

static void buf_append_n(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    if (n > 0) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_append(Buf *b, const char *s) { buf_append_n(b, s, strlen(s)); }

static void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    memcpy(p, s, n + 1);
    return p;
}

/* ---------------------------------------------------------------- filesystem helpers */

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, (size_t)sz, f);
    data[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return data;
}

static bool write_file_all(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    return ok;
}

static void mkdir_p(const char *path) {
    char *copy = xstrdup(path);
    size_t len = strlen(copy);
    for (size_t i = 0; i < len; i++) {
        if ((copy[i] == '/' || copy[i] == '\\') && i > 0) {
            if (i == 2 && copy[1] == ':') continue;
            char saved = copy[i];
            copy[i] = '\0';
#ifdef _WIN32
            _mkdir(copy);
#else
            mkdir(copy, 0755);
#endif
            copy[i] = saved;
        }
    }
#ifdef _WIN32
    _mkdir(copy);
#else
    mkdir(copy, 0755);
#endif
    free(copy);
}

static void rm_rf(const char *path) {
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                rm_rf(child);
            } else {
                DeleteFileA(child);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(child);
        else remove(child);
    }
    closedir(d);
    rmdir(path);
#endif
}

typedef struct {
    char **items;
    int count;
    int cap;
} StrList;

static void sl_init(StrList *l) { l->items = NULL; l->count = 0; l->cap = 0; }

static void sl_push(StrList *l, char *s) {
    if (l->count >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        char **ni = realloc(l->items, (size_t)nc * sizeof(char *));
        if (!ni) { fprintf(stderr, "out of memory\n"); exit(2); }
        l->items = ni;
        l->cap = nc;
    }
    l->items[l->count++] = s;
}

static void sl_free(StrList *l) {
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int str_ptr_cmp(const void *a, const void *b) {
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static void list_files_with_ext(const char *dir, const char *ext, StrList *out) {
    size_t elen = strlen(ext);
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            size_t nlen = strlen(fd.cFileName);
            if (nlen >= elen && strcmp(fd.cFileName + nlen - elen, ext) == 0) {
                sl_push(out, xstrdup(fd.cFileName));
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen >= elen && strcmp(ent->d_name + nlen - elen, ext) == 0) {
                sl_push(out, xstrdup(ent->d_name));
            }
        }
        closedir(d);
    }
#endif
    if (out->count > 1) {
        qsort(out->items, (size_t)out->count, sizeof(char *), str_ptr_cmp);
    }
}

static int count_files_in_dir(const char *dir, const char *suffix_filter) {
    int count = 0;
    size_t slen = suffix_filter ? strlen(suffix_filter) : 0;
#ifdef _WIN32
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            size_t nlen = strlen(fd.cFileName);
            if (!suffix_filter || (nlen >= slen && strcmp(fd.cFileName + nlen - slen, suffix_filter) == 0)) {
                count++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            size_t nlen = strlen(ent->d_name);
            if (!suffix_filter || (nlen >= slen && strcmp(ent->d_name + nlen - slen, suffix_filter) == 0)) {
                count++;
            }
        }
        closedir(d);
    }
#endif
    return count;
}

/* ---------------------------------------------------------------- process runner */

typedef struct {
    int exit_code;
    Buf out;
    Buf err;
} ProcResult;

static void proc_result_free(ProcResult *r) {
    buf_free(&r->out);
    buf_free(&r->err);
}

#ifdef _WIN32
typedef struct {
    HANDLE handle;
    Buf *buf;
} PipeReaderArgs;

static DWORD WINAPI pipe_reader_thread(LPVOID param) {
    PipeReaderArgs *args = (PipeReaderArgs *)param;
    char tmp[4096];
    DWORD nread = 0;
    while (ReadFile(args->handle, tmp, sizeof(tmp), &nread, NULL) && nread > 0) {
        buf_append_n(args->buf, tmp, (size_t)nread);
    }
    return 0;
}

static void append_win_quoted(Buf *cmd, const char *arg) {
    bool need_quote = (arg[0] == '\0');
    for (const char *p = arg; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') { need_quote = true; break; }
    }
    if (!need_quote) { buf_append(cmd, arg); return; }
    buf_append(cmd, "\"");
    for (const char *p = arg; *p; p++) {
        if (*p == '"') buf_append(cmd, "\\\"");
        else buf_append_n(cmd, p, 1);
    }
    buf_append(cmd, "\"");
}

static bool run_process(const char *const *argv, ProcResult *res) {
    buf_init(&res->out);
    buf_init(&res->err);
    buf_append(&res->out, "");
    buf_append(&res->err, "");
    res->exit_code = -1;

    SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) return false;
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) { CloseHandle(out_r); CloseHandle(out_w); return false; }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    Buf cmd;
    buf_init(&cmd);
    for (int i = 0; argv[i]; i++) {
        if (i > 0) buf_append(&cmd, " ");
        if (i == 0) {
            /* CreateProcess does not resolve a relative program path written
               with forward slashes, so hand it the native separator. */
            char *prog = xstrdup(argv[0]);
            for (char *p = prog; *p; p++) {
                if (*p == '/') *p = '\\';
            }
            append_win_quoted(&cmd, prog);
            free(prog);
        } else {
            append_win_quoted(&cmd, argv[i]);
        }
    }

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w;
    si.hStdError = err_w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessA(NULL, cmd.data, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    buf_free(&cmd);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        return false;
    }

    PipeReaderArgs out_args = {out_r, &res->out};
    PipeReaderArgs err_args = {err_r, &res->err};
    HANDLE t_out = CreateThread(NULL, 0, pipe_reader_thread, &out_args, 0, NULL);
    HANDLE t_err = CreateThread(NULL, 0, pipe_reader_thread, &err_args, 0, NULL);

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (t_out) { WaitForSingleObject(t_out, INFINITE); CloseHandle(t_out); }
    if (t_err) { WaitForSingleObject(t_err, INFINITE); CloseHandle(t_err); }

    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    res->exit_code = (int)code;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(out_r);
    CloseHandle(err_r);
    return true;
}
#else
static bool run_process(const char *const *argv, ProcResult *res) {
    buf_init(&res->out);
    buf_init(&res->err);
    buf_append(&res->out, "");
    buf_append(&res->err, "");
    res->exit_code = -1;

    int op[2], ep[2];
    if (pipe(op) != 0) return false;
    if (pipe(ep) != 0) { close(op[0]); close(op[1]); return false; }

    pid_t pid = fork();
    if (pid < 0) {
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        return false;
    }
    if (pid == 0) {
        dup2(op[1], STDOUT_FILENO);
        dup2(ep[1], STDERR_FILENO);
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(op[1]);
    close(ep[1]);
    char tmp[4096];
    ssize_t n;
    while ((n = read(op[0], tmp, sizeof(tmp))) > 0) buf_append_n(&res->out, tmp, (size_t)n);
    while ((n = read(ep[0], tmp, sizeof(tmp))) > 0) buf_append_n(&res->err, tmp, (size_t)n);
    close(op[0]);
    close(ep[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    res->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return true;
}
#endif

/* ---------------------------------------------------------------- case specification */

typedef struct {
    char code[16];
    int line;
    int col;
    char *contains;
} ExpectedDiag;

typedef struct {
    StrList out;
    char exit_spec[32];
    StrList stderr_contains;
    StrList c_stderr_contains;
    StrList c_contains;
    StrList c_lacks;
    ExpectedDiag *errors;
    int error_count;
    ExpectedDiag *warnings;
    int warning_count;
    bool strict;
} CaseSpec;

static void spec_init(CaseSpec *s) {
    memset(s, 0, sizeof(*s));
    sl_init(&s->out);
    strcpy(s->exit_spec, "0");
    sl_init(&s->stderr_contains);
    sl_init(&s->c_stderr_contains);
    sl_init(&s->c_contains);
    sl_init(&s->c_lacks);
}

static void spec_free(CaseSpec *s) {
    sl_free(&s->out);
    sl_free(&s->stderr_contains);
    sl_free(&s->c_stderr_contains);
    sl_free(&s->c_contains);
    sl_free(&s->c_lacks);
    for (int i = 0; i < s->error_count; i++) free(s->errors[i].contains);
    free(s->errors);
    for (int i = 0; i < s->warning_count; i++) free(s->warnings[i].contains);
    free(s->warnings);
}

static char *trim_inplace(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static void add_expected_diag(ExpectedDiag **arr, int *count, const char *val) {
    /* Format: FCnnnn line:col text */
    char code[16];
    int line = 0, col = 0;
    int consumed = 0;
    if (sscanf(val, "%15s %d:%d %n", code, &line, &col, &consumed) < 3) {
        fprintf(stderr, "bad diagnostic directive: %s\n", val);
        exit(2);
    }
    *arr = realloc(*arr, (size_t)(*count + 1) * sizeof(ExpectedDiag));
    strcpy((*arr)[*count].code, code);
    (*arr)[*count].line = line;
    (*arr)[*count].col = col;
    (*arr)[*count].contains = xstrdup(val + consumed);
    (*count)++;
}

static void spec_parse_file(const char *path, CaseSpec *spec) {
    spec_init(spec);
    char *text = read_file_all(path, NULL);
    if (!text) return;

    char *p = text;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        char *line = trim_inplace(p);
        if (strncmp(line, "//!", 3) == 0) {
            char *colon = strchr(line + 3, ':');
            if (colon) {
                *colon = '\0';
                char *key = trim_inplace(line + 3);
                char *val = trim_inplace(colon + 1);
                if (strcmp(key, "out") == 0) sl_push(&spec->out, xstrdup(val));
                else if (strcmp(key, "exit") == 0) { strncpy(spec->exit_spec, val, sizeof(spec->exit_spec) - 1); }
                else if (strcmp(key, "stderr-contains") == 0) sl_push(&spec->stderr_contains, xstrdup(val));
                else if (strcmp(key, "c-stderr-contains") == 0) sl_push(&spec->c_stderr_contains, xstrdup(val));
                else if (strcmp(key, "c-contains") == 0) sl_push(&spec->c_contains, xstrdup(val));
                else if (strcmp(key, "c-lacks") == 0) sl_push(&spec->c_lacks, xstrdup(val));
                else if (strcmp(key, "flags") == 0 && strcmp(val, "--strict") == 0) spec->strict = true;
                else if (strcmp(key, "error") == 0) add_expected_diag(&spec->errors, &spec->error_count, val);
                else if (strcmp(key, "warning") == 0) add_expected_diag(&spec->warnings, &spec->warning_count, val);
                /* fs-contains / fs-lacks ignored for C target */
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(text);
}

/* ---------------------------------------------------------------- parsed diagnostics */

typedef struct {
    char *file;
    int line;
    int col;
    bool is_warning;
    char code[16];
    char *msg;
} ParsedDiag;

typedef struct {
    ParsedDiag *items;
    int count;
} ParsedDiagList;

static void pdl_free(ParsedDiagList *l) {
    for (int i = 0; i < l->count; i++) {
        free(l->items[i].file);
        free(l->items[i].msg);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static void parse_diagnostics(const char *stderr_text, ParsedDiagList *out) {
    out->items = NULL;
    out->count = 0;
    char *copy = xstrdup(stderr_text);
    char *p = copy;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(p);
        if (len > 0 && p[len - 1] == '\r') p[--len] = '\0';

        /* Match: <file>(<line>,<col>): <error|warning> <FCnnnn>: <msg> */
        char *sep = strstr(p, "): ");
        if (sep) {
            char *lparen = sep;
            while (lparen > p && *lparen != '(') lparen--;
            if (*lparen == '(') {
                int line = 0, col = 0;
                if (sscanf(lparen + 1, "%d,%d", &line, &col) == 2) {
                    char sev[16], code[16];
                    int consumed = 0;
                    if (sscanf(sep + 3, "%15s %15[^:]: %n", sev, code, &consumed) >= 2
                        && (strcmp(sev, "error") == 0 || strcmp(sev, "warning") == 0)
                        && strncmp(code, "FC", 2) == 0)
                    {
                        out->items = realloc(out->items, (size_t)(out->count + 1) * sizeof(ParsedDiag));
                        ParsedDiag *d = &out->items[out->count++];
                        size_t flen = (size_t)(lparen - p);
                        d->file = malloc(flen + 1);
                        memcpy(d->file, p, flen);
                        d->file[flen] = '\0';
                        d->line = line;
                        d->col = col;
                        d->is_warning = (strcmp(sev, "warning") == 0);
                        strncpy(d->code, code, sizeof(d->code) - 1);
                        d->code[sizeof(d->code) - 1] = '\0';
                        d->msg = xstrdup(sep + 3 + consumed);
                    }
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(copy);
}

static bool diag_matches(const ParsedDiag *a, const ExpectedDiag *e) {
    return strcmp(a->code, e->code) == 0
        && a->line == e->line
        && a->col == e->col
        && strstr(a->msg, e->contains) != NULL;
}

/* ---------------------------------------------------------------- test results */

typedef struct {
    char *name;
    char *category;
    bool passed;
    char *detail;
    double seconds;
} TestRecord;

static TestRecord *g_records = NULL;
static int g_rec_count = 0;
static const char *g_fc2c = "./fc2c.exe";
static const char *g_cc = "gcc";
static const char *g_filter = NULL;

static double now_sec(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static bool match_filter(const char *name) {
    if (!g_filter) return true;
    /* case-insensitive substring */
    size_t nlen = strlen(name), flen = strlen(g_filter);
    for (size_t i = 0; i + flen <= nlen; i++) {
        size_t j = 0;
        while (j < flen && tolower((unsigned char)name[i + j]) == tolower((unsigned char)g_filter[j])) j++;
        if (j == flen) return true;
    }
    return false;
}

static void record_result(const char *name, const char *category, bool passed, const char *detail, double secs) {
    g_records = realloc(g_records, (size_t)(g_rec_count + 1) * sizeof(TestRecord));
    g_records[g_rec_count].name = xstrdup(name);
    g_records[g_rec_count].category = xstrdup(category);
    g_records[g_rec_count].passed = passed;
    g_records[g_rec_count].detail = xstrdup(detail);
    g_records[g_rec_count].seconds = secs;
    g_rec_count++;

    if (passed) {
        printf("[PASS] %s (%.1fs)\n", name, secs);
    } else {
        const char *nl = strchr(detail, '\n');
        int first_len = nl ? (int)(nl - detail) : (int)strlen(detail);
        printf("[FAIL] %s (%.1fs) -- %.*s\n", name, secs, first_len, detail);
    }
    fflush(stdout);
}

static const char *map_stderr_for_c(const char *frag) {
    if (strcmp(frag, "System.DivideByZeroException") == 0) return "division by zero";
    if (strcmp(frag, "System.OverflowException") == 0) return "overflow";
    return frag;
}

/* ---------------------------------------------------------------- runners */

static void run_program_case(const char *test_name, const char *input, const char *work_name, const CaseSpec *spec) {
    if (!match_filter(test_name)) return;
    double t0 = now_sec();
    Buf err;
    buf_init(&err);

    char dir[512], c_out[544], exe_out[544];
    snprintf(dir, sizeof(dir), "tests/work/%s", work_name);
    mkdir_p(dir);
    snprintf(c_out, sizeof(c_out), "%s/program.c", dir);
#ifdef _WIN32
    snprintf(exe_out, sizeof(exe_out), "%s/program.exe", dir);
#else
    snprintf(exe_out, sizeof(exe_out), "%s/program", dir);
#endif

    const char *t_argv[6];
    int ti = 0;
    t_argv[ti++] = g_fc2c;
    t_argv[ti++] = input;
    t_argv[ti++] = "--out";
    t_argv[ti++] = c_out;
    if (spec->strict) t_argv[ti++] = "--strict";
    t_argv[ti] = NULL;

    ProcResult tr;
    if (!run_process(t_argv, &tr)) {
        buf_printf(&err, "failed to launch %s", g_fc2c);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }

    if (tr.exit_code != 0) {
        buf_printf(&err, "translation of %s failed (exit %d):\n%s", input, tr.exit_code, tr.err.data);
        proc_result_free(&tr);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }

    ParsedDiagList warnings;
    parse_diagnostics(tr.err.data, &warnings);
    for (int i = 0; i < warnings.count; i++) {
        if (!warnings.items[i].is_warning) {
            buf_printf(&err, "a successful translation reported an error:\n%s", tr.err.data);
            pdl_free(&warnings);
            proc_result_free(&tr);
            record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
            buf_free(&err);
            return;
        }
    }
    for (int i = 0; i < spec->warning_count; i++) {
        bool found = false;
        for (int j = 0; j < warnings.count; j++) {
            if (diag_matches(&warnings.items[j], &spec->warnings[i])) { found = true; break; }
        }
        if (!found) {
            buf_printf(&err, "expected warning %s at %d:%d containing \"%s\"\nactual stderr:\n%s",
                spec->warnings[i].code, spec->warnings[i].line, spec->warnings[i].col,
                spec->warnings[i].contains, tr.err.data);
            pdl_free(&warnings);
            proc_result_free(&tr);
            record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
            buf_free(&err);
            return;
        }
    }
    if (warnings.count != spec->warning_count) {
        buf_printf(&err, "expected %d warning(s) but got %d:\n%s", spec->warning_count, warnings.count, tr.err.data);
        pdl_free(&warnings);
        proc_result_free(&tr);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }
    proc_result_free(&tr);

    char *gen_c = read_file_all(c_out, NULL);
    if (!gen_c) {
        buf_printf(&err, "could not read generated C file %s", c_out);
        pdl_free(&warnings);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }
    for (int i = 0; i < spec->c_contains.count; i++) {
        if (!strstr(gen_c, spec->c_contains.items[i])) {
            buf_printf(&err, "generated C does not contain '%s'", spec->c_contains.items[i]);
            free(gen_c);
            pdl_free(&warnings);
            record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
            buf_free(&err);
            return;
        }
    }
    for (int i = 0; i < spec->c_lacks.count; i++) {
        if (strstr(gen_c, spec->c_lacks.items[i])) {
            buf_printf(&err, "generated C should not contain '%s'", spec->c_lacks.items[i]);
            free(gen_c);
            pdl_free(&warnings);
            record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
            buf_free(&err);
            return;
        }
    }
    free(gen_c);

    const char *cc_argv[] = {g_cc, "-std=c17", "-Wall", "-Wextra", "-Werror", c_out, "-o", exe_out, NULL};
    ProcResult cc;
    if (!run_process(cc_argv, &cc) || cc.exit_code != 0) {
        buf_printf(&err, "C build failed (exit %d):\n%s%s", cc.exit_code, cc.out.data, cc.err.data);
        proc_result_free(&cc);
        pdl_free(&warnings);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }
    proc_result_free(&cc);

    const char *run_argv[] = {exe_out, NULL};
    ProcResult run;
    if (!run_process(run_argv, &run)) {
        buf_printf(&err, "failed to run %s", exe_out);
        pdl_free(&warnings);
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }

    Buf expected_out;
    buf_init(&expected_out);
    buf_append(&expected_out, "");
    for (int i = 0; i < spec->out.count; i++) {
        buf_append(&expected_out, spec->out.items[i]);
        buf_append(&expected_out, "\n");
    }

    bool ok = true;
    if (strcmp(run.out.data, expected_out.data) != 0) {
        buf_printf(&err, "stdout mismatch for %s\nexpected:\n%sactual:\n%s", input, expected_out.data, run.out.data);
        ok = false;
    } else if (strcmp(spec->exit_spec, "nonzero") == 0) {
        if (run.exit_code == 0) {
            buf_printf(&err, "expected nonzero exit code but got 0");
            ok = false;
        }
    } else {
        int exp_exit = atoi(spec->exit_spec);
        if (run.exit_code != exp_exit) {
            buf_printf(&err, "expected exit code %d but got %d\nstderr:\n%s", exp_exit, run.exit_code, run.err.data);
            ok = false;
        }
    }

    if (ok) {
        const StrList *check_list = (spec->c_stderr_contains.count > 0) ? &spec->c_stderr_contains : &spec->stderr_contains;
        for (int i = 0; i < check_list->count; i++) {
            const char *frag = (spec->c_stderr_contains.count > 0) ? check_list->items[i] : map_stderr_for_c(check_list->items[i]);
            if (!strstr(run.err.data, frag)) {
                buf_printf(&err, "stderr does not contain '%s':\n%s", frag, run.err.data);
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        Buf summary;
        buf_init(&summary);
        buf_printf(&summary, "exit code %d; stdout ", run.exit_code);
        if (run.out.len == 0) {
            buf_append(&summary, "(empty)");
        } else {
            for (size_t i = 0; i < run.out.len; i++) {
                if (run.out.data[i] == '\n') {
                    if (i + 1 < run.out.len) buf_append(&summary, " | ");
                } else {
                    buf_append_n(&summary, &run.out.data[i], 1);
                }
            }
        }
        if (warnings.count > 0) {
            buf_printf(&summary, "; %d translator warning(s)", warnings.count);
        }
        record_result(test_name, "end-to-end", true, summary.data, now_sec() - t0);
        buf_free(&summary);
    } else {
        record_result(test_name, "end-to-end", false, err.data, now_sec() - t0);
    }

    buf_free(&expected_out);
    proc_result_free(&run);
    pdl_free(&warnings);
    buf_free(&err);
}

static void run_reject_case(const char *test_name, const char *input, const char *work_name, const CaseSpec *spec) {
    if (!match_filter(test_name)) return;
    double t0 = now_sec();
    Buf err;
    buf_init(&err);

    if (spec->error_count == 0) {
        buf_printf(&err, "%s has no '//! error:' directive", input);
        record_result(test_name, "translator rejects", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }

    char dir[512], c_out[544];
    snprintf(dir, sizeof(dir), "tests/work/%s", work_name);
    mkdir_p(dir);
    snprintf(c_out, sizeof(c_out), "%s/program.c", dir);

    const char *t_argv[6];
    int ti = 0;
    t_argv[ti++] = g_fc2c;
    t_argv[ti++] = input;
    t_argv[ti++] = "--out";
    t_argv[ti++] = c_out;
    if (spec->strict) t_argv[ti++] = "--strict";
    t_argv[ti] = NULL;

    ProcResult tr;
    if (!run_process(t_argv, &tr)) {
        buf_printf(&err, "failed to launch %s", g_fc2c);
        record_result(test_name, "translator rejects", false, err.data, now_sec() - t0);
        buf_free(&err);
        return;
    }

    bool ok = true;
    if (tr.exit_code != 1) {
        buf_printf(&err, "expected exit code 1 but got %d\n%s%s", tr.exit_code, tr.out.data, tr.err.data);
        ok = false;
    } else if (file_exists(c_out)) {
        buf_printf(&err, "output file was written even though translation failed");
        ok = false;
    } else if (count_files_in_dir(dir, NULL) != 0) {
        buf_printf(&err, "failed translation left files behind in %s", dir);
        ok = false;
    }

    ParsedDiagList diags;
    parse_diagnostics(tr.err.data, &diags);
    if (ok && diags.count == 0) {
        buf_printf(&err, "no diagnostics in expected 'file(line,col): error FCnnnn: message' format:\n%s", tr.err.data);
        ok = false;
    }
    for (int i = 0; ok && i < diags.count; i++) {
        if (strcmp(diags.items[i].file, input) != 0) {
            buf_printf(&err, "diagnostic names file '%s' instead of '%s'", diags.items[i].file, input);
            ok = false;
        }
    }
    for (int i = 0; ok && i < spec->error_count; i++) {
        bool found = false;
        for (int j = 0; j < diags.count; j++) {
            if (!diags.items[j].is_warning && diag_matches(&diags.items[j], &spec->errors[i])) {
                found = true;
                break;
            }
        }
        if (!found) {
            buf_printf(&err, "expected %s at %d:%d containing \"%s\"\nactual stderr:\n%s",
                spec->errors[i].code, spec->errors[i].line, spec->errors[i].col,
                spec->errors[i].contains, tr.err.data);
            ok = false;
        }
    }

    if (ok) {
        record_result(test_name, "translator rejects", true, tr.err.data, now_sec() - t0);
    } else {
        record_result(test_name, "translator rejects", false, err.data, now_sec() - t0);
    }

    pdl_free(&diags);
    proc_result_free(&tr);
    buf_free(&err);
}

/* ---------------------------------------------------------------- extra CLI / translator tests */

static void test_failed_preserves_output(void) {
    const char *name = "cli: failed translation leaves the existing output file untouched";
    if (!match_filter(name)) return;
    double t0 = now_sec();
    mkdir_p("tests/work/cli-preserve");
    const char *out = "tests/work/cli-preserve/program.c";

    const char *a1[] = {g_fc2c, "examples/sum_squares.fc", "--out", out, NULL};
    ProcResult r1;
    run_process(a1, &r1);
    if (r1.exit_code != 0) {
        record_result(name, "cli", false, r1.err.data, now_sec() - t0);
        proc_result_free(&r1);
        return;
    }
    proc_result_free(&r1);

    size_t len_before = 0;
    char *before = read_file_all(out, &len_before);

    const char *bad_sem = "tests/work/cli-preserve/bad.fc";
    const char *bad_src = "int main()\n{\n    let x = 1;\n    x = 2;\n    return x;\n}\n";
    write_file_all(bad_sem, bad_src, strlen(bad_src));
    const char *a2[] = {g_fc2c, bad_sem, "--out", out, NULL};
    ProcResult r2;
    run_process(a2, &r2);

    const char *bad_syn = "tests/work/cli-preserve/syntax.fc";
    const char *syn_src = "int main()\n{\n    return 0\n}\n";
    write_file_all(bad_syn, syn_src, strlen(syn_src));
    const char *a3[] = {g_fc2c, bad_syn, "--out", out, NULL};
    ProcResult r3;
    run_process(a3, &r3);

    size_t len_after = 0;
    char *after = read_file_all(out, &len_after);
    int tmps = count_files_in_dir("tests/work/cli-preserve", ".tmp");

    bool ok = (r2.exit_code == 1) && (r3.exit_code == 1)
        && before && after && len_before == len_after && memcmp(before, after, len_before) == 0
        && tmps == 0;

    record_result(name, "cli", ok,
        ok ? "output unchanged after semantic and syntax errors; 0 leftover .tmp files"
           : "output changed, wrong exit code, or leftover .tmp file",
        now_sec() - t0);

    free(before);
    free(after);
    proc_result_free(&r2);
    proc_result_free(&r3);
}

static void test_usage_and_io_errors(void) {
    const char *name = "cli: usage and I/O errors give distinct exit codes";
    if (!match_filter(name)) return;
    double t0 = now_sec();
    mkdir_p("tests/work/cli-usage");

    const char *c0[] = {g_fc2c, NULL};
    const char *c1[] = {g_fc2c, "examples/sum_squares.fc", NULL};
    const char *c2[] = {g_fc2c, "examples/sum_squares.fc", "--out", NULL};
    const char *c3[] = {g_fc2c, "--bogus", NULL};
    const char *c4[] = {g_fc2c, "--strict", NULL};
    const char *c5[] = {g_fc2c, "a.fc", "b.fc", "--out", "tests/work/cli-usage/c.c", NULL};
    const char *c6[] = {g_fc2c, "tests/does-not-exist.fc", "--out", "tests/work/cli-usage/x.c", NULL};
    const char *c7[] = {g_fc2c, "--help", NULL};

    const char *const *cases[] = {c0, c1, c2, c3, c4, c5, c6, c7};
    int expected[] = {2, 2, 2, 2, 2, 2, 3, 0};

    bool ok = true;
    Buf detail;
    buf_init(&detail);
    for (int i = 0; i < 8; i++) {
        ProcResult r;
        run_process(cases[i], &r);
        if (r.exit_code != expected[i]) {
            buf_printf(&detail, "case %d exited %d, expected %d", i, r.exit_code, expected[i]);
            ok = false;
            proc_result_free(&r);
            break;
        }
        proc_result_free(&r);
    }
    if (ok && file_exists("tests/work/cli-usage/x.c")) {
        buf_append(&detail, "output file created for missing input");
        ok = false;
    }
    if (ok) buf_append(&detail, "all 8 CLI cases returned expected exit codes (0/2/3)");
    record_result(name, "cli", ok, detail.data, now_sec() - t0);
    buf_free(&detail);
}

static void test_deterministic_output(void) {
    const char *name = "translator: output is deterministic";
    if (!match_filter(name)) return;
    double t0 = now_sec();
    mkdir_p("tests/work/det");
    const char *o1 = "tests/work/det/a.c";
    const char *o2 = "tests/work/det/b.c";
    const char *a1[] = {g_fc2c, "tests/cases/run/evaluation_order.fc", "--out", o1, NULL};
    const char *a2[] = {g_fc2c, "tests/cases/run/evaluation_order.fc", "--out", o2, NULL};
    ProcResult r1, r2;
    run_process(a1, &r1);
    run_process(a2, &r2);
    size_t l1 = 0, l2 = 0;
    char *s1 = read_file_all(o1, &l1);
    char *s2 = read_file_all(o2, &l2);
    bool ok = (r1.exit_code == 0 && r2.exit_code == 0 && s1 && s2 && l1 == l2 && memcmp(s1, s2, l1) == 0);
    char msg[128];
    snprintf(msg, sizeof(msg), "%zu bytes, identical across runs", l1);
    record_result(name, "translator", ok, ok ? msg : "two translations differ", now_sec() - t0);
    free(s1);
    free(s2);
    proc_result_free(&r1);
    proc_result_free(&r2);
}

static void test_sum_squares_shape(void) {
    const char *name = "translator: sum_squares emits lifted lambda, const let, and separate C main";
    if (!match_filter(name)) return;
    double t0 = now_sec();
    mkdir_p("tests/work/shape");
    const char *out = "tests/work/shape/sum_squares.c";
    const char *argv[] = {g_fc2c, "examples/sum_squares.fc", "--out", out, NULL};
    ProcResult r;
    run_process(argv, &r);
    char *code = read_file_all(out, NULL);
    const char *req[] = {
        "static inline int32_t fc_lam_sumSquares_1_square(const int32_t u_x)",
        "int32_t u_sumSquares(int32_t u_n)",
        "int32_t u_main(void)",
        "const int32_t u_result = u_sumSquares(5);",
        "int main(void)",
        "return (int)u_main();",
    };
    bool ok = (r.exit_code == 0 && code != NULL);
    const char *missing = NULL;
    for (int i = 0; ok && i < 6; i++) {
        if (!strstr(code, req[i])) { ok = false; missing = req[i]; }
    }
    char msg[256];
    if (ok) snprintf(msg, sizeof(msg), "found lifted static inline lambda, const let binding, and separate C main(void)");
    else snprintf(msg, sizeof(msg), "missing expected fragment: %s", missing ? missing : "(translation failed)");
    record_result(name, "translator", ok, msg, now_sec() - t0);
    free(code);
    proc_result_free(&r);
}

/* ---------------------------------------------------------------- report & main */

static void write_report(const char *path, int passed, int failed, double elapsed) {
    Buf sb;
    buf_init(&sb);
    buf_printf(&sb, "Passed: **%d**, failed: **%d**, skipped: **0**, total: %d (%.1fs, native C17 harness).\n\n",
        passed, failed, passed + failed, elapsed);

    for (int pass = 0; pass < 2; pass++) {
        bool want_pass = (pass == 1);
        int cnt = 0;
        for (int i = 0; i < g_rec_count; i++) if (g_records[i].passed == want_pass) cnt++;
        if (cnt == 0) continue;
        buf_printf(&sb, "#### %s (%d)\n\n", want_pass ? "Passed" : "Failed", cnt);
        for (int i = 0; i < g_rec_count; i++) {
            if (g_records[i].passed != want_pass) continue;
            buf_printf(&sb, "- **%s** [%s, %.1fs]\n\n  ```text\n", g_records[i].name, g_records[i].category, g_records[i].seconds);
            const char *p = g_records[i].detail;
            while (*p) {
                const char *nl = strchr(p, '\n');
                size_t len = nl ? (size_t)(nl - p) : strlen(p);
                if (len > 0 && p[len - 1] == '\r') len--;
                buf_append(&sb, "  ");
                buf_append_n(&sb, p, len);
                buf_append(&sb, "\n");
                if (!nl) break;
                p = nl + 1;
            }
            buf_append(&sb, "  ```\n\n");
        }
    }
    write_file_all(path, sb.data, sb.len);
    buf_free(&sb);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    g_fc2c = "./fc2c.exe";
#else
    g_fc2c = "./fc2c";
#endif
    const char *report_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) report_path = argv[++i];
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) g_filter = argv[++i];
        else if (strcmp(argv[i], "--fc2c") == 0 && i + 1 < argc) g_fc2c = argv[++i];
        else if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc) g_cc = argv[++i];
        else {
            fprintf(stderr, "unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    rm_rf("tests/work");
    mkdir_p("tests/work");

    double started = now_sec();

    /* 1. examples (.fc files) */
    {
        CaseSpec ss;
        spec_init(&ss);
        sl_push(&ss.out, xstrdup("55"));
        run_program_case("example: examples/sum_squares.fc prints 55", "examples/sum_squares.fc", "example-sum_squares", &ss);
        spec_free(&ss);

        StrList examples;
        sl_init(&examples);
        list_files_with_ext("examples", ".fc", &examples);
        for (int i = 0; i < examples.count; i++) {
            if (strcmp(examples.items[i], "sum_squares.fc") == 0) continue;
            char rel[256], tname[300], wname[256];
            snprintf(rel, sizeof(rel), "examples/%s", examples.items[i]);
            snprintf(tname, sizeof(tname), "example: %s", rel);
            char stem[128];
            strncpy(stem, examples.items[i], sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            snprintf(wname, sizeof(wname), "example-%s", stem);

            CaseSpec spec;
            spec_parse_file(rel, &spec);
            run_program_case(tname, rel, wname, &spec);
            spec_free(&spec);
        }
        sl_free(&examples);
    }

    /* 2. tests/cases/run (.fc files) */
    {
        StrList runs;
        sl_init(&runs);
        list_files_with_ext("tests/cases/run", ".fc", &runs);
        for (int i = 0; i < runs.count; i++) {
            char rel[256], tname[300], wname[256], stem[128];
            snprintf(rel, sizeof(rel), "tests/cases/run/%s", runs.items[i]);
            strncpy(stem, runs.items[i], sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            snprintf(tname, sizeof(tname), "run: %s", stem);
            snprintf(wname, sizeof(wname), "run-%s", stem);

            CaseSpec spec;
            spec_parse_file(rel, &spec);
            run_program_case(tname, rel, wname, &spec);
            spec_free(&spec);
        }
        sl_free(&runs);
    }

    /* 3. tests/cases/reject (.fc files) */
    {
        StrList rejects;
        sl_init(&rejects);
        list_files_with_ext("tests/cases/reject", ".fc", &rejects);
        for (int i = 0; i < rejects.count; i++) {
            char rel[256], tname[256], wname[256], stem[128];
            snprintf(rel, sizeof(rel), "tests/cases/reject/%s", rejects.items[i]);
            strncpy(stem, rejects.items[i], sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';
            snprintf(tname, sizeof(tname), "reject: %s", stem);
            snprintf(wname, sizeof(wname), "reject-%s", stem);

            CaseSpec spec;
            spec_parse_file(rel, &spec);
            run_reject_case(tname, rel, wname, &spec);
            spec_free(&spec);
        }
        sl_free(&rejects);
    }

    /* 4. CLI & translator tests */
    test_failed_preserves_output();
    test_usage_and_io_errors();
    test_deterministic_output();
    test_sum_squares_shape();

    double elapsed = now_sec() - started;
    int passed = 0, failed = 0;
    for (int i = 0; i < g_rec_count; i++) {
        if (g_records[i].passed) passed++;
        else failed++;
    }

    printf("\nPassed: %d  Failed: %d  Skipped: 0  Total: %d  (%.1fs)\n", passed, failed, g_rec_count, elapsed);
    for (int i = 0; i < g_rec_count; i++) {
        if (!g_records[i].passed) {
            printf("\nFAILED: %s\n%s\n", g_records[i].name, g_records[i].detail);
        }
    }

    if (report_path) {
        write_report(report_path, passed, failed, elapsed);
        printf("Report written to %s\n", report_path);
    }

    for (int i = 0; i < g_rec_count; i++) {
        free(g_records[i].name);
        free(g_records[i].category);
        free(g_records[i].detail);
    }
    free(g_records);
    return failed == 0 ? 0 : 1;
}
