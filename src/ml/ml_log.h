#pragma once
#ifndef PHANTOM_ML_LOG_H
#define PHANTOM_ML_LOG_H

#include "../config.h"
#include <stdarg.h>
#include <stdio.h>

/* Usage: #define MY_LOG(...) ml_log("prefix", __VA_ARGS__) */
#if PHANTOM_DEBUG
static void ml_log(const char *prefix, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void ml_log(const char *prefix, const char *fmt, ...)
{
    fprintf(stderr, "%s: ", prefix);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
#else
static void ml_log(const char *prefix, const char *fmt, ...)
{
    (void)prefix;
    (void)fmt;
}
#endif

#endif /* PHANTOM_ML_LOG_H */
