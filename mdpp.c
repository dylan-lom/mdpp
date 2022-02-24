#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#include <sys/wait.h>

#define SV_IMPLEMENTATION
#include "sv.h"

enum {
    PIPE_READ = 0,
    PIPE_WRITE
};

typedef struct {
    FILE *src;
    FILE *dest;
    FILE *shell_write;
    FILE *shell_read;
    bool dest_is_pipe;
    bool in_code_block;

    bool header_is_open;
} Context;

void
die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

bool
next_line(String_View *sv, FILE *stream)
{
    errno = 0;
    char *lineptr = NULL;
    size_t n = 0;
    int read = getline(&lineptr, &n, stream); 
    if (errno != 0) {
        die("ERROR: Unable to read next line: %s\n", strerror(errno));
    }

    if (read < 1) return false;

    *sv = sv_trim_right(sv_from_parts(lineptr, read));
    return true;
}

void
shell_exec(Context *ctx, String_View command, String_View *result)
{
    fprintf(ctx->shell_write, SV_Fmt ";\n", SV_Arg(command));
    fflush(ctx->shell_write);
    // TODO: Handle multi-line shell return?
    if (result) {
        next_line(result, ctx->shell_read);
    } else {
        fflush(ctx->shell_read);
    }
}

void
shell_set(Context *ctx, String_View name, String_View val)
{
    fprintf(ctx->shell_write, SV_Fmt "='" SV_Fmt "';\n", SV_Arg(name),
            SV_Arg(val));
    fflush(ctx->shell_write);
}

String_View
execute(String_View command)
{
    char *cmd = calloc(command.count, sizeof(*cmd));
    strncpy(cmd, command.data, command.count);
    FILE *pp = popen(cmd, "r");
    if (pp == NULL) {
        // TODO: The popen() function does not set errno if memory allocation fails.
        die("ERROR: Unable to open pipe to command " SV_Fmt ": %s\n",
            SV_Arg(command), strerror(errno));
    }

    // TODO: Handle multi-line shell return?
    String_View result;
    next_line(&result, pp);
    pclose(pp);
    return result;
}

bool
index_of_delim(String_View sv, String_View delim, size_t *index)
{
    // Could not find
    size_t n = 0;
    if (!sv_find(sv, delim, &n)) return false;

    // Found an escaped occurence
    if (n > 0 && *(sv.data + n - 1) == '\\') {
        n += delim.count;
        sv_chop_left(&sv, n);

        size_t nindex = 0;
        if (!index_of_delim(sv, delim, &nindex)) return false;

        if (index) *index = n + nindex;
        return true;
    }

    if (index) *index = n;
    return true;
}

void
preprocess_shell(Context *ctx, String_View sv)
{
    String_View result;
    shell_exec(ctx, sv, &result);
    fprintf(ctx->dest, SV_Fmt, SV_Arg(result));
}

void
preprocess_tex(Context *ctx, String_View sv)
{
    fprintf(ctx->dest, "<djl-tex>" SV_Fmt "</djl-tex>", SV_Arg(sv));
}

void
preprocess_head(Context *ctx, String_View sv)
{
    ctx->header_is_open = !ctx->header_is_open;
    fprintf(ctx->dest, ctx->header_is_open ? "<head>" : "</head>");
    (void)sv;
}

void
preprocess_title(Context *ctx, String_View sv)
{
    fprintf(ctx->dest, "<title>" SV_Fmt "</title>", SV_Arg(sv));
    // Set $title in shell
    shell_set(ctx, SV("title"), sv);
}

void
preprocess_meta(Context *ctx, String_View sv)
{
    // TODO: Support spaces
    size_t n;
    sv_index_of(sv, ' ', &n);
    if (!n) die("ERROR: %meta directive requires two arguments\n");
    String_View name = sv_chop_left(&sv, n);
    String_View val = sv_trim(sv);

    fprintf(ctx->dest, "<meta name=\"" SV_Fmt "\" content=\"" SV_Fmt "\">",
            SV_Arg(name), SV_Arg(val));
    shell_set(ctx, name, val);
}

typedef void (*Directive_Handler)(Context *ctx, String_View sv);
typedef struct {
    String_View open;
    String_View close;
    Directive_Handler handler;
} Directive;

#define DIRECTIVES_COUNT 5
Directive directives[DIRECTIVES_COUNT] = {
    // shell
    {
        .open = SV_STATIC("$("),
        .close = SV_STATIC(")"),
        .handler = preprocess_shell,
    },
    // djl-tex
    {
        .open = SV_STATIC("$$"),
        .close = SV_STATIC("$$"),
        .handler = preprocess_tex,
    },
    // title
    {
        .open = SV_STATIC("%title "),
        .handler = preprocess_title,
    },
    // meta
    {
        .open = SV_STATIC("%meta "),
        .handler = preprocess_meta,
    },
    // head
    {
        .open = SV_STATIC("%"),
        .handler = preprocess_head,
    },
};

String_View
get_enclosed(Directive dir, String_View *sv)
{
    size_t index = 0;
    if (!index_of_delim(*sv, dir.close, &index)) {
        die("ERROR: Directive " SV_Fmt "..." SV_Fmt " was not closed!\n",
            SV_Arg(dir.open), SV_Arg(dir.close));
    }

    String_View content = sv_chop_left(sv, index);

    if (!sv_eq(dir.open, dir.close)) {
        String_View slice = content;
        // Nested directive found
        // TODO: Do we really want to support nesting?
        while (index_of_delim(slice, dir.open, &index)) {
            // Find something to close it
            if (!index_of_delim(*sv, dir.close, &index)) {
                die("ERROR: Directive " SV_Fmt "..." SV_Fmt " was not closed!\n",
                            SV_Arg(dir.open), SV_Arg(dir.close));
            }
            // Extend cmd to enclose closing delim
            slice = sv_chop_left(sv, index + dir.close.count);
            content.count += slice.count;
        }
    }

    return sv_trim_right(content);
}


void
preprocess(Context *ctx)
{
    String_View in;

    FILE *src = ctx->src;
    FILE *dest = ctx->dest;

    while (next_line(&in, src)) {
        String_View sv = in;

        if (sv_starts_with(sv, SV("    ")) || sv_starts_with(sv, SV("	"))) {
            ctx->in_code_block = true;
        } else if (ctx->in_code_block) {
            ctx->in_code_block = false;
        }

        // Whole-line directives
        for (size_t i = 0; i < DIRECTIVES_COUNT; i++) {
            if (directives[i].close.count != 0) continue;

            if (sv_starts_with(sv, directives[i].open)) {
                sv_chop_left(&sv, directives[i].open.count);
                directives[i].handler(ctx, sv);
                sv.count = 0; // Done parsing this line!
                break;
            }
        }

        // If we're in a code block we can just print the whole line and be done
        // with it
        if (ctx->in_code_block) {
            fprintf(dest, SV_Fmt, SV_Arg(sv));
            sv.count = 0;
        }

        while (sv.count > 0) {
            bool processed = false;

            // In-line directives
            for (size_t i = 0; i < DIRECTIVES_COUNT; i++) {
                if (directives[i].close.count == 0) continue;

                if (sv_starts_with(sv, directives[i].open)) {
                    sv_chop_left(&sv, directives[i].open.count);
                    String_View content = get_enclosed(directives[i], &sv);
                    directives[i].handler(ctx, content);
                    sv_chop_left(&sv, directives[i].close.count);
                    processed = true;
                    break;
                }
            }

            if (!processed) {
                String_View chopped = sv_chop_left(&sv, 1);

                if (sv_eq(chopped, SV("\\"))) {
                    // Make sure we only unescape if we recognise a directive
                    // following the backslash
                    for (size_t i = 0; i < DIRECTIVES_COUNT; i++) {
                        Directive dir = directives[i];
                        if (sv_starts_with(sv, dir.open)) {
                            chopped = sv_chop_left(&sv, dir.open.count);
                            break;
                        } else if (dir.close.count && sv_starts_with(sv, dir.close)) {
                            chopped = sv_chop_left(&sv, dir.close.count);
                            break;
                        }
                    }
                }

                fprintf(dest, SV_Fmt, SV_Arg(chopped));
            }
        }

        fputc('\n', dest);
        free((char*)in.data);
    }
}

void
usage(const char *progname)
{
    die("USAGE: %s [-e] [src [dest]]\n", progname);
}

Context
init(int argc, const char *argv[])
{
    const char *progname = argv[0];
    argc -= 1;
    argv += 1;

    Context ctx = {0};
    ctx.src = stdin;
    ctx.dest = stdout;

    // Create shell subprocess
    {
        int shfd[4];
        if (pipe(shfd) < 0 || pipe(shfd+2) < 0) {
            die("ERROR: Unable to create pipes for shell: %s\n",
                strerror(errno));
        }

        pid_t p = fork();
        if (p < 0) die("ERROR: Unable to fork: %s\n", strerror(errno));
        if (p == 0) {
            if (dup2(shfd[PIPE_READ], fileno(stdin)) < 0) {
                die("ERROR: Unable to set stdin in child: %s\n",
                    strerror(errno));
            }

            if (dup2(shfd[2+PIPE_WRITE], fileno(stdout)) < 0) {
                die("ERROR: Unable to set stdout in child: %s\n",
                    strerror(errno));
            }

            if (close(shfd[PIPE_READ]) < 0
                    || close(shfd[PIPE_WRITE]) < 0
                    || close(shfd[2+PIPE_READ]) < 0
                    || close(shfd[2+PIPE_WRITE]) < 0) {
                die("ERROR: Unable to close pipe fd's: %s\n", strerror(errno));

            }

            if (execl("/bin/sh", "/bin/sh", (char*)NULL) < 0) {
                die("ERROR: Unable to execute shell: %s\n", strerror(errno));
            }

            assert(false && "UNREACHABLE");
        } else {
            if (close(shfd[PIPE_READ]) < 0 || close(shfd[2+PIPE_WRITE])) {
                die("ERROR: Unable to close pipe fd's: %s\n", strerror(errno));
            }

            ctx.shell_write = fdopen(shfd[PIPE_WRITE], "w");
            ctx.shell_read = fdopen(shfd[2+PIPE_READ], "r");
            if (ctx.shell_write == NULL || ctx.shell_read == NULL) {
                die("ERROR: Unable to open FILEs from pipes to shell: %s\n",
                    strerror(errno));
            }
        }
    }

    // Flags
    bool flag_e = false;
    int i;
    for (i = 0; i < argc; i++) {
        if (argv[i][0] != '-') break;
        if (strcmp(argv[i], "-e") == 0) {
            flag_e = true;
        } else {
            usage(progname);
        }
    }

    argc -= i;
    argv += i;

    // Arguments
    if (argc > 2) usage(progname);
    if (argc > 0) {
        ctx.src = fopen(argv[0], "r");
        if (ctx.src == NULL) {
            die("ERROR: Unable to open src file `%s`: %s\n", argv[0],
                strerror(errno));
        }
    }

    if (argc > 1) {
        ctx.dest = fopen(argv[1], "w");
        if (ctx.dest == NULL) {
            die("ERROR: Unable to open dest file `%s`: %s\n", argv[1],
                strerror(errno));
        }
    }

    if (flag_e) {
        assert(ctx.dest != NULL);
        int mdfd[2];
        if (pipe(mdfd) < 0) {
            die("ERROR: Unable to create pipes: %s\n", strerror(errno));
        }

        pid_t p = fork();
        if (p < 0) die("ERROR: Unable to fork: %s\n", strerror(errno));

        if (p == 0) {
            if (dup2(mdfd[PIPE_READ], fileno(stdin)) < 0) {
                die("ERROR: Unable to set stdin of child: %s\n",
                    strerror(errno));
            }
            if (close(mdfd[PIPE_READ]) < 0 || close(mdfd[PIPE_WRITE]) < 0) {
                die("ERROR: Unable to close pipe fd's: %s\n", strerror(errno));
            }

            if (fileno(ctx.dest) != fileno(stdout)) {
                if (dup2(fileno(ctx.dest), fileno(stdout)) < 0) {
                    die("ERROR: Unable to set stdout of child: %s\n",
                        strerror(errno));
                }
                if (close(fileno(ctx.dest)) < 0) {
                    die("ERROR: Unable to close dest fd after dup2: %s\n",
                        strerror(errno));
                }
            }
            // TODO: Take markdown command from args
            char *args[] = { "markdown", NULL };
            if (execvp(args[0], args) < 0) {
                die("ERROR: Unable to exec' markdown command: `%s`: %s\n",
                    args[0], strerror(errno));
            }
            assert(false && "UNREACHABLE");
        }

        // Parent
        if (close(mdfd[PIPE_READ]) < 0) {
            die("ERROR: Unable to close pipe: %s\n", strerror(errno));
        }
        ctx.dest = fdopen(mdfd[PIPE_WRITE], "w");
        if (ctx.dest == NULL) {
            die("ERROR: Unable to open FILE to subprocess: %s\n",
                strerror(errno));
        }
        ctx.dest_is_pipe = true;
    }

    return ctx;
}

void
cleanup(Context *ctx)
{
    fclose(ctx->src);
    ctx->src = NULL;
    if (ctx->dest_is_pipe) {
        if (pclose(ctx->dest) < 0) {
            die("ERROR: Unable to close destination pipe: %s\n",
                strerror(errno));
        }
    } else {
        fclose(ctx->dest);
    }
    ctx->dest = NULL;

    if (pclose(ctx->shell_write) < 0) {
        die("ERROR: Unable to close shell_write pipe: %s\n",
            strerror(errno));
    }

    if (pclose(ctx->shell_read) < 0) {
        die("ERROR: Unable to close shell_read pipe: %s\n",
            strerror(errno));
    }

    // Wait for all children to finish
    while (wait(NULL) != -1) {}
}

// Pre-process markdown input from stdin
int
main(int argc, const char *argv[])
{
    Context ctx = init(argc, argv);
    preprocess(&ctx);
    cleanup(&ctx);
}

