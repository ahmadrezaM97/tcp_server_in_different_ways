#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void fatal_error(const char *msg) {
    fprintf(stderr, "Fatal error: %s (%s)\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static inline void usage_error(const char *progname, const char *usage) {
    fprintf(stderr, "Usage: %s %s\n", progname, usage);
    exit(EXIT_FAILURE);
}
