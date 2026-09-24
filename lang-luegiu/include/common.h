#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>

#define NL fprintf(out, "\n")
#define SPACES fprintf(out, "    ")
#define NLSTDOUT fprintf(stdout, "\n")

typedef struct {
    const char* start;
    size_t len;
} View;
