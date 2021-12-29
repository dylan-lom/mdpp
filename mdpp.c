#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SV_IMPLEMENTATION
#include "sv.h"

bool
nextline(String_View *sv, FILE *stream)
{
    errno = 0;
    char *lineptr = NULL;
    size_t n = 0;
    int read = getline(&lineptr, &n, stream); 
    if (errno != 0) {
        fprintf(stderr, "ERROR: Unable to read next line: %s\n",
                strerror(errno));
        exit(1);
    }

    if (read < 1) return false;

    *sv = sv_trim_right(sv_from_parts(lineptr, read));
    return true;
}

String_View
slurp(FILE *fp)
{
    /* TODO: If we are actually going to alloc this shit we should probably at
     * least realloc when we're out of space... */
    const size_t buflen = 2048; 
    char *buf = calloc(buflen, sizeof(*buf));
    memset(buf, '\0', buflen);
    size_t bufsz = 0;

    for (bufsz = 0; bufsz < buflen; bufsz++) {
        buf[bufsz] = fgetc(fp);
        if (buf[bufsz] == EOF) {
            break;
        }
    }

    return sv_from_parts(buf, bufsz);
}

String_View
execute(String_View command)
{
    char *cmd = calloc(command.count, sizeof(*cmd));
    strncpy(cmd, command.data, command.count);
    FILE *pp = popen(cmd, "r");
    if (pp == NULL) {
        // TODO: The popen() function does not set errno if memory allocation fails.
        fprintf(stderr,
                "ERROR: Unable to open pipe to command " SV_Fmt ": %s\n",
                SV_Arg(command), strerror(errno));
        exit(1);
    }

    // TODO: Implement nextline()
    char *buf = calloc(1028, sizeof(*buf));
    if (buf == NULL) { exit(1); }
    if (fgets(buf, 1028, pp) == NULL) { exit(1); }

    return sv_trim_right(sv_from_parts(buf, strlen(buf)));
}

// Pre-process markdown input from stdin
int
main(void)
{
    String_View in;
    String_View sh_open = sv_from_cstr("$(");
    String_View sh_close = sv_from_cstr(")");

    while (nextline(&in, stdin)) {
        String_View sv = in;

        while (sv.count > 0) {
            if (sv_starts_with(sv, sh_open)) {
                sv_chop_left(&sv, sh_open.count);
                size_t index = 0;
                if (!sv_find(sv, sh_close, &index)) {
                    fprintf(stderr, "ERROR: Shell substring was not closed!\n");
                    exit(1);
                }
                String_View cmd = sv_chop_left(&sv, index);
                String_View result = execute(cmd);
                printf("[[[" SV_Fmt "]]]", SV_Arg(result));
                sv_chop_left(&sv, sh_close.count); // Advance past closing delim
            } else {
                printf("%c", *sv.data);
                sv_chop_left(&sv, 1);
            }
        }

        putchar('\n');
        free((char*)in.data);
    }
}

