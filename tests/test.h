/**
 * @file test.h
 * @brief Minimal header-only check helpers for the host unit tests.
 *
 * Each test file defines `static void test_xxx(void)` functions, calls them
 * with RUN() from main(), and returns test_finish(). A failed check prints
 * `file:line: expr (got ..., expected ...)` and the test keeps going, so one
 * run reports every failure.
 */

#ifndef WSA_TEST_H
#define WSA_TEST_H

#include <stdio.h>
#include <string.h>

static unsigned long test_checks_;
static unsigned long test_failures_;

/** Record one check; print the location and detail when it failed. */
#define TEST_RECORD_(ok, ...)                                         \
    do {                                                              \
        test_checks_++;                                               \
        if (!(ok)) {                                                  \
            test_failures_++;                                         \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

#define CHECK(cond) TEST_RECORD_((cond), "%s", #cond)

#define CHECK_EQ_INT(a, b)                                            \
    do {                                                              \
        long long a_ = (long long)(a), b_ = (long long)(b);           \
        TEST_RECORD_(a_ == b_, "%s == %s (got %lld, expected %lld)",  \
                     #a, #b, a_, b_);                                 \
    } while (0)

#define CHECK_STR(a, b)                                               \
    do {                                                              \
        const char *a_ = (a), *b_ = (b);                              \
        int ok_ = a_ && b_ && strcmp(a_, b_) == 0;                    \
        TEST_RECORD_(ok_, "%s == %s (got \"%s\", expected \"%s\")",   \
                     #a, #b, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                         \
    do {                                                              \
        double a_ = (double)(a), b_ = (double)(b);                    \
        double d_ = a_ > b_ ? a_ - b_ : b_ - a_;                      \
        TEST_RECORD_(d_ <= (double)(eps),                             \
                     "%s ~= %s (got %.6f, expected %.6f)",            \
                     #a, #b, a_, b_);                                 \
    } while (0)

/** Run one test function and print its name. */
#define RUN(fn)                                                       \
    do {                                                              \
        printf("-- %s\n", #fn);                                       \
        fflush(stdout);                                               \
        fn();                                                         \
    } while (0)

/** Print the summary; return the process exit code (0 when all passed). */
static inline int test_finish(void)
{
    printf("%lu checks, %lu failed\n", test_checks_, test_failures_);
    return test_failures_ ? 1 : 0;
}

#endif /* WSA_TEST_H */
