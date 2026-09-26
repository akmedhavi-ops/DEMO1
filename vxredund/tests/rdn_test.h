/* rdn_test.h - minimal self-contained test harness */
#ifndef RDN_TEST_H
#define RDN_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int g_checks;
static int g_failures;

#define CHECK(c) do {                                                        \
        g_checks++;                                                          \
        if (!(c)) {                                                          \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) do {                                                  \
        long long va_ = (long long)(a), vb_ = (long long)(b);                \
        g_checks++;                                                          \
        if (va_ != vb_) {                                                    \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld != %lld)\n",       \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                   \
        }                                                                    \
    } while (0)

#define RUN(fn) do {                                                         \
        int before_ = g_failures;                                            \
        printf("  %-44s", #fn);                                              \
        fflush(stdout);                                                      \
        fn();                                                                \
        printf("%s\n", (g_failures == before_) ? "ok" : "FAILED");           \
    } while (0)

static int test_report(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}

#endif
