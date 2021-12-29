#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define SV_IMPLEMENTATION
#include "sv.h"

bool
next_line(String_View *sv, FILE *stream)
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
preprocess(FILE *src, FILE *dst)
{
    String_View in;
    String_View sh_open = sv_from_cstr("$(");
    String_View sh_close = sv_from_cstr(")");

    while (next_line(&in, src)) {
        String_View sv = in;

        while (sv.count > 0) {
            if (sv_starts_with(sv, sh_open)) {
                sv_chop_left(&sv, sh_open.count);
                size_t index = 0;
                if (!index_of_delim(sv, sh_close, &index)) {
                    fprintf(stderr, "ERROR: Shell substring was not closed!\n");
                    exit(1);
                }
                String_View cmd = unescape(sv_chop_left(&sv, index));
                String_View result = execute(cmd);
                fprintf(dst, SV_Fmt, SV_Arg(result));
                sv_chop_left(&sv, sh_close.count); // Advance past closing delim
            } else {
                String_View chopped = sv_chop_left(&sv, 1);
                fprintf(dst, SV_Fmt, SV_Arg(chopped));
                if (sv_eq(chopped, SV("\\"))) {
                    chopped = sv_chop_left(&sv, 1);
                    fprintf(dst, SV_Fmt, SV_Arg(chopped));
                }
            }
        }

        fputc('\n', dst);
        free((char*)in.data);
    }
}

// Pre-process markdown input from stdin
int
main(void)
{
    preprocess(stdin, stdout);
}

