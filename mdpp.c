#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#define SV_IMPLEMENTATION
#include "sv.h"

enum {
    PIPE_READ = 0,
    PIPE_WRITE
};

typedef struct {
    FILE *src;
    FILE *dest;
    bool dest_is_pipe;
    bool in_code_block;
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
    if (!sv_find(sv, delim, index)) return false;

    // Found an escaped occurence
    if (*index > 0 && *(sv.data + *index - 1) == '\\') {
        *index += delim.count;
        sv_chop_left(&sv, *index);

        size_t nindex = 0;
        if (!index_of_delim(sv, delim, &nindex)) return false;

        *index += nindex;
        return true;
    }

    return true;
}

void
preprocess(Context *ctx)
{
    String_View in;
    String_View sh_open = sv_from_cstr("$(");
    String_View sh_close = sv_from_cstr(")");

    FILE *src = ctx->src,
         *dest = ctx->dest;

    while (next_line(&in, src)) {
        String_View sv = in;

        if (sv_starts_with(sv, SV("    ")) || sv_starts_with(sv, SV("	"))) {
            ctx->in_code_block = true;
        } else if (ctx->in_code_block) {
            ctx->in_code_block = false;
        }

        while (sv.count > 0) {
            if (!ctx->in_code_block && sv_starts_with(sv, sh_open)) {
                sv_chop_left(&sv, sh_open.count);
                size_t index = 0;
                if (!index_of_delim(sv, sh_close, &index)) {
                    die("ERROR: Shell substring was not closed!\n");
                }
                String_View cmd = unescape(sv_chop_left(&sv, index));
                String_View result = execute(cmd);
                fprintf(dest, SV_Fmt, SV_Arg(result));
                sv_chop_left(&sv, sh_close.count); // Advance past closing delim
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

FILE *
cmd_open(String_View command, FILE *dest)
{
    assert(dest != NULL);

    char *cmd = calloc(command.count + 1, sizeof(*cmd));
    if (cmd == NULL) {
        die("ERROR: Unable to allocate memory for command `" SV_Fmt "`: %s\n",
            SV_Arg(command), strerror(errno));
    }
    strncpy(cmd, command.data, command.count + 1);

    int fds[2], child_in, child_out;
    if (pipe(fds) < 0) {
        die("ERROR: Unable to create pipe for command `%s`: %s\n", cmd,
            strerror(errno));
    }

    child_in = fds[PIPE_READ];
    child_out = fileno(dest);

    pid_t pid = fork();
    if (pid < 0) {
        die("ERROR: Unable to create child process for command `%s`: %s\n",
            cmd, strerror(errno));
    }

    // In parent -- return write end of pipe.
    if (pid > 0) {
        close(fds[PIPE_READ]); // Close unused read end in parent
        FILE *pp = fdopen(fds[PIPE_WRITE], "w");
        if (pp == NULL) {
            die("ERROR: Unable to create FILE* for command pipe: %s\n",
                strerror(errno));
        }
        return pp;
    }

    // In child -- setup stdin / stdout and become command
    if (dup2(child_in, STDIN_FILENO) < 0 || dup2(child_out, STDOUT_FILENO) < 0) {
        // TODO: The parent will not die if the child fails to start...
        die("ERROR: Unable to set stdin/stdout in child process: %s\n",
            strerror(errno));
    };

    // Close extra fd's (making sure we don't accidentally close stdin/stdout)
    close(fds[PIPE_WRITE]);
    if (child_in != STDIN_FILENO) close(child_in);
    if (child_out != STDOUT_FILENO) close(child_out);

    char *args[] = { cmd, NULL };
    if (execvp(args[0], args) < 0) {
        die("ERROR: Unable to exec command `%s`: %s\n", cmd, strerror(errno));
    }

    assert(false && "UNREACHABLE");
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
        // TODO: Take markdown command from args
        ctx.dest = cmd_open(SV("markdown"), ctx.dest);
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
}

// Pre-process markdown input from stdin
int
main(int argc, const char *argv[])
{
    Context ctx = init(argc, argv);
    preprocess(&ctx);
    cleanup(&ctx);
}

