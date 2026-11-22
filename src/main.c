#include "fc.h"

#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define FC_MKDIR(p) _mkdir(p)
#define FC_GETPID() _getpid()
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define FC_MKDIR(p) mkdir((p), 0755)
#define FC_GETPID() getpid()
#endif

/* ---------------------------------------------------------------- arena */

#define ARENA_DEFAULT_CAP 65536

void arena_init(Arena *a) {
    a->head = NULL;
}

void *arena_alloc(Arena *a, size_t size) {
    size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned == 0) aligned = 8;
    if (!a->head || a->head->used + aligned > a->head->cap) {
        size_t cap = aligned > ARENA_DEFAULT_CAP ? aligned : ARENA_DEFAULT_CAP;
        ArenaBlock *b = malloc(sizeof(ArenaBlock) + cap);
        if (!b) {
            fprintf(stderr, "out of memory\n");
            exit(3);
        }
        b->next = a->head;
        b->used = 0;
        b->cap = cap;
        a->head = b;
    }
    void *ptr = a->head->data + a->head->used;
    a->head->used += aligned;
    return ptr;
}

char *arena_strndup(Arena *a, const char *s, size_t len) {
    char *copy = arena_alloc(a, len + 1);
    if (len > 0) memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *arena_strdup(Arena *a, const char *s) {
    return arena_strndup(a, s, strlen(s));
}

char *arena_printf(Arena *a, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    size_t sz = (n > 0) ? (size_t)n + 1 : 1;
    char *buf = arena_alloc(a, sz);
    vsnprintf(buf, sz, fmt, ap2);
    va_end(ap2);
    return buf;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b);
        b = next;
    }
    a->head = NULL;
}

/* ---------------------------------------------------------------- string buffer */

void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_reserve(StrBuf *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap * 2 : 256;
    while (nc < sb->len + extra + 1) nc *= 2;
    char *nd = realloc(sb->data, nc);
    if (!nd) {
        fprintf(stderr, "out of memory\n");
        exit(3);
    }
    sb->data = nd;
    sb->cap = nc;
}

void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    sb_reserve(sb, n);
    if (n > 0) memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s) {
    sb_append_n(sb, s, strlen(s));
}

void sb_append_char(StrBuf *sb, char c) {
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) {
        va_end(ap2);
        return;
    }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
}

void sb_line(StrBuf *sb, const char *s) {
    if (s && s[0]) sb_append(sb, s);
    sb_append_char(sb, '\n');
}

/* ---------------------------------------------------------------- diagnostics & helpers */

void diag_init(DiagList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void diag_add(DiagList *list, Arena *a, const char *code, SourceLoc loc, bool is_warning, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    size_t sz = (n > 0) ? (size_t)n + 1 : 1;
    char *msg = arena_alloc(a, sz);
    vsnprintf(msg, sz, fmt, ap2);
    va_end(ap2);

    if (list->count >= list->cap) {
        int nc = list->cap ? list->cap * 2 : 16;
        Diagnostic *ni = realloc(list->items, (size_t)nc * sizeof(Diagnostic));
        if (!ni) {
            fprintf(stderr, "out of memory\n");
            exit(3);
        }
        list->items = ni;
        list->cap = nc;
    }
    Diagnostic *d = &list->items[list->count];
    d->code = code;
    d->message = msg;
    d->loc = loc;
    d->is_warning = is_warning;
    d->order = list->count;
    list->count++;
}

static int diag_cmp(const void *pa, const void *pb) {
    const Diagnostic *a = (const Diagnostic *)pa;
    const Diagnostic *b = (const Diagnostic *)pb;
    if (a->loc.line != b->loc.line) return a->loc.line - b->loc.line;
    if (a->loc.column != b->loc.column) return a->loc.column - b->loc.column;
    return a->order - b->order;
}

void diag_sort(DiagList *list) {
    if (list->count > 1) {
        qsort(list->items, (size_t)list->count, sizeof(Diagnostic), diag_cmp);
    }
}

bool diag_has_errors(const DiagList *list) {
    for (int i = 0; i < list->count; i++) {
        if (!list->items[i].is_warning) return true;
    }
    return false;
}

int diag_error_count(const DiagList *list) {
    int n = 0;
    for (int i = 0; i < list->count; i++) {
        if (!list->items[i].is_warning) n++;
    }
    return n;
}

void diag_print(const Diagnostic *d, FILE *out) {
    fprintf(out, "%s(%d,%d): %s %s: %s\n",
        d->loc.file, d->loc.line, d->loc.column,
        d->is_warning ? "warning" : "error",
        d->code, d->message);
}

void diag_free(DiagList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

const char *fc_type_name(FcType t) {
    switch (t) {
        case FC_TYPE_INT: return "int";
        case FC_TYPE_BOOL: return "bool";
        case FC_TYPE_CHAR: return "char";
        case FC_TYPE_UCHAR: return "unsigned char";
        default: return "<error>";
    }
}

const char *binop_source(BinaryOp op) {
    switch (op) {
        case BINOP_ADD: return "+";
        case BINOP_SUB: return "-";
        case BINOP_MUL: return "*";
        case BINOP_DIV: return "/";
        case BINOP_REM: return "%";
        case BINOP_LT: return "<";
        case BINOP_LE: return "<=";
        case BINOP_GT: return ">";
        case BINOP_GE: return ">=";
        case BINOP_EQ: return "==";
        case BINOP_NE: return "!=";
        case BINOP_AND: return "&&";
        case BINOP_OR: return "||";
    }
    return "?";
}

const char *assignop_source(AssignOp op) {
    switch (op) {
        case ASSIGN_SET: return "=";
        case ASSIGN_ADD: return "+=";
        case ASSIGN_SUB: return "-=";
        case ASSIGN_MUL: return "*=";
        case ASSIGN_DIV: return "/=";
        case ASSIGN_REM: return "%=";
        case ASSIGN_INC: return "++";
        case ASSIGN_DEC: return "--";
    }
    return "=";
}

/* ---------------------------------------------------------------- file I/O & atomic write */

static char *read_entire_file(const char *path, int *out_errno) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        *out_errno = errno;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        *out_errno = errno;
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        *out_errno = errno;
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        *out_errno = ENOMEM;
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)len, f);
    if (ferror(f)) {
        *out_errno = errno;
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[nread] = '\0';
    fclose(f);

    /* Strip optional UTF-8 BOM. */
    if (nread >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, nread - 3 + 1);
    }
    return buf;
}

static void ensure_parent_dirs(const char *path) {
    size_t len = strlen(path);
    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, path, len + 1);

    for (size_t i = 0; i < len; i++) {
        if ((copy[i] == '/' || copy[i] == '\\') && i > 0) {
            /* Skip Windows drive prefix like "C:\" */
            if (i == 2 && copy[1] == ':') continue;
            char saved = copy[i];
            copy[i] = '\0';
            (void)FC_MKDIR(copy);
            copy[i] = saved;
        }
    }
    free(copy);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *p = slash > bslash ? slash : bslash;
    return p ? p + 1 : path;
}

static bool write_file_atomic(const char *output_path, const char *data, size_t len, int *out_errno) {
    ensure_parent_dirs(output_path);

    const char *bname = base_name(output_path);
    size_t dir_len = (size_t)(bname - output_path);
    StrBuf tmp_path;
    sb_init(&tmp_path);
    if (dir_len > 0) sb_append_n(&tmp_path, output_path, dir_len);
    sb_printf(&tmp_path, ".%s.%d.tmp", bname, (int)FC_GETPID());

    FILE *f = fopen(tmp_path.data, "wb");
    if (!f) {
        *out_errno = errno;
        sb_free(&tmp_path);
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    int close_rc = fclose(f);
    if (written != len || close_rc != 0) {
        *out_errno = errno ? errno : EIO;
        remove(tmp_path.data);
        sb_free(&tmp_path);
        return false;
    }

#ifdef _WIN32
    if (!MoveFileExA(tmp_path.data, output_path, MOVEFILE_REPLACE_EXISTING)) {
        *out_errno = EIO;
        remove(tmp_path.data);
        sb_free(&tmp_path);
        return false;
    }
#else
    if (rename(tmp_path.data, output_path) != 0) {
        *out_errno = errno;
        remove(tmp_path.data);
        sb_free(&tmp_path);
        return false;
    }
#endif

    sb_free(&tmp_path);
    return true;
}

/* ---------------------------------------------------------------- CLI */

enum {
    EXIT_OK = 0,
    EXIT_SOURCE_ERRORS = 1,
    EXIT_USAGE_ERROR = 2,
    EXIT_IO_ERROR = 3
};

static const char *USAGE =
    "Functional C -> C17 translator (fc2c, native C17, version 1.4)\n"
    "\n"
    "Usage:\n"
    "  fc2c <input.fc> --out <output.c> [--strict]\n"
    "\n"
    "Options:\n"
    "  --strict   Reject what C accepts but a careful programmer avoids: a number stored in a bool,\n"
    "             unreachable code, a function that can reach its end without returning, and a\n"
    "             variable declared without a value. Without it the first is converted as in C,\n"
    "             the next two are warnings, and such a variable starts at 0.\n"
    "\n"
    "Translates a Functional C source file into portable C17. Compile the result with any C17 compiler, e.g.\n"
    "  fc2c examples/sum_squares.fc --out generated/sum_squares.c\n"
    "  gcc -std=c17 -Wall -Wextra -Werror generated/sum_squares.c -o generated/sum_squares.exe\n"
    "\n"
    "Exit codes: 0 translated, 1 source errors, 2 bad command line, 3 file I/O failure.\n";

static int usage_error(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    fprintf(stderr, "usage: fc2c <input.fc> --out <output.c> [--strict]   (use --help for details)\n");
    return EXIT_USAGE_ERROR;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "/?") == 0)) {
        fputs(USAGE, stdout);
        return EXIT_OK;
    }

    const char *input = NULL;
    const char *output = NULL;
    bool strict = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--strict") == 0) {
            strict = true;
        } else if (strcmp(argv[i], "--out") == 0) {
            if (i + 1 >= argc) return usage_error("'--out' must be followed by an output file path");
            if (output) return usage_error("'--out' was given more than once");
            output = argv[++i];
        } else if (argv[i][0] == '-') {
            char buf[256];
            snprintf(buf, sizeof(buf), "unknown option '%s'", argv[i]);
            return usage_error(buf);
        } else if (!input) {
            input = argv[i];
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "unexpected extra argument '%s'; only one input file is supported", argv[i]);
            return usage_error(buf);
        }
    }

    if (!input) return usage_error("no input file was given");
    if (!output) return usage_error("no output file was given; use '--out <output.c>'");

    int io_err = 0;
    char *source = read_entire_file(input, &io_err);
    if (!source) {
        fprintf(stderr, "error FC0001: cannot read input file '%s': %s\n", input, strerror(io_err));
        return EXIT_IO_ERROR;
    }

    Arena arena;
    arena_init(&arena);
    DiagList diags;
    diag_init(&diags);

    TokenList tokens = {NULL, 0, 0};
    bool ok = fc_lex(&arena, source, input, &tokens, &diags);
    ProgramNode *prog = NULL;
    if (ok) {
        prog = fc_parse(&arena, &tokens, input, &diags);
        ok = (prog != NULL);
    }
    if (ok) {
        fc_check(&arena, prog, strict, &diags);
        ok = !diag_has_errors(&diags);
    }

    for (int i = 0; i < diags.count; i++) {
        diag_print(&diags.items[i], stderr);
    }

    int exit_code = EXIT_OK;
    if (!ok) {
        fprintf(stderr, "translation failed with %d error(s); '%s' was not written (an existing file is left unchanged)\n",
            diag_error_count(&diags), output);
        exit_code = EXIT_SOURCE_ERRORS;
    } else {
        StrBuf emitted;
        sb_init(&emitted);
        fc_emit_c(&arena, prog, base_name(input), &emitted);
        if (!write_file_atomic(output, emitted.data, emitted.len, &io_err)) {
            fprintf(stderr, "error FC0002: cannot write output file '%s': %s\n", output, strerror(io_err));
            exit_code = EXIT_IO_ERROR;
        } else {
            fprintf(stdout, "Translated %s -> %s\n", input, output);
        }
        sb_free(&emitted);
    }

    free(tokens.items);
    diag_free(&diags);
    arena_free(&arena);
    free(source);
    return exit_code;
}
