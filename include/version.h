/**
 * @file version.h
 * @brief Phoenix SDR version information
 * 
 * Include this in any tool/executable and call print_version() at startup.
 */

#ifndef PHOENIX_VERSION_H
#define PHOENIX_VERSION_H

#define PHOENIX_VERSION_MAJOR   0
#define PHOENIX_VERSION_MINOR   2
#define PHOENIX_VERSION_PATCH   0
#define PHOENIX_VERSION_STRING  "0.2.0"

/* Build timestamp - set by compiler */
#define PHOENIX_BUILD_DATE      __DATE__
#define PHOENIX_BUILD_TIME      __TIME__

#include <stdio.h>

static inline void print_version(const char *tool_name) {
    printf("%s v%s (built %s %s)\n", 
           tool_name, PHOENIX_VERSION_STRING, 
           PHOENIX_BUILD_DATE, PHOENIX_BUILD_TIME);
}

#endif /* PHOENIX_VERSION_H */
