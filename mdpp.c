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

// TODO: Only unescape recognised sequences
String_View
unescape(String_View sv)
{
    size_t index = 0;
    if (!sv_index_of(sv, '\\', &index)) return sv;

    char *data = calloc(sv.count, sizeof(*data));
    size_t count = 0;
    do {
        memcpy(data + count, sv.data, index);
        count += index;
        data[count++] = sv.data[index + 1];
        sv_chop_left(&sv, index + 2);
    } while (sv_index_of(sv, '\\', &index));

    memcpy(data + count, sv.data, sv.count);
    count += sv.count;
    return sv_from_parts(data, count);
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

typedef struct {
    String_View open;
    String_View close;
} Directive;

// TODO: Create a table of delimiters
Directive shell = {
    .open = SV_STATIC("$("),
    .close = SV_STATIC(")"),
};

Directive head = {
    .open = SV_STATIC("%"),
};

Directive title = {
    .open = SV_STATIC("%title "),
};

Directive meta = {
    .open = SV_STATIC("%meta "),
};

String_View
parse_substitution(String_View *sv)
{
    size_t index = 0;
    if (!index_of_delim(*sv, shell.close, &index)) {
        die("ERROR: Shell substring was not closed!\n");
    }

    String_View cmd = sv_chop_left(sv, index);
    String_View slice = cmd;
    // Nested substitutions found
    while (index_of_delim(slice, shell.open, &index)) {
        // Find something to close it
        if (!index_of_delim(*sv, shell.close, &index)) {
            die("ERROR: Shell substring was not closed!\n");
        }
        // Extend cmd to enclose closing delim
        slice = sv_chop_left(sv, index + shell.close.count);
        cmd.count += slice.count;
    }

    return unescape(sv_trim_right(cmd));
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

        if (sv_eq(sv_trim_right(sv), head.open)) {
            ctx->header_is_open = !ctx->header_is_open;
            fprintf(dest, ctx->header_is_open ? "<head>" : "</head>");
            sv.count = 0; // Ignore trailing whitespace
        }

        if (sv_starts_with(sv, title.open)) {
            sv_chop_left(&sv, title.open.count);
            fprintf(dest, "<title>" SV_Fmt "</title>", SV_Arg(sv));
            // Set $title in shell
            shell_set(ctx, SV("title"), sv);
            sv.count = 0; // Done parsing this line
        } else if (sv_starts_with(sv, meta.open)) {
            sv_chop_left(&sv, meta.open.count);
            // TODO: Support spaces
            size_t n;
            sv_index_of(sv, ' ', &n);
            if (!n) die("ERROR: %meta directive requires two arguments\n");
            String_View name = sv_chop_left(&sv, n);
            String_View val = sv_trim(sv);

            fprintf(dest, "<meta name=\"" SV_Fmt "\" content=\"" SV_Fmt "\">",
                    SV_Arg(name), SV_Arg(val));
            shell_set(ctx, name, val);

            sv.count = 0;
        }


        while (sv.count > 0) {
            if (!ctx->in_code_block && sv_starts_with(sv, shell.open)) {
                sv_chop_left(&sv, shell.open.count);
                String_View cmd = parse_substitution(&sv);
                String_View result;
                shell_exec(ctx, cmd, &result);
                fprintf(dest, SV_Fmt, SV_Arg(result));
                sv_chop_left(&sv, shell.close.count); // Advance past closing delim
            } else {
                // TODO: We should only escape directives we define.
                String_View chopped = sv_chop_left(&sv, 1);
                if (sv_eq(chopped, SV("\\"))) chopped = sv_chop_left(&sv, 1);
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

