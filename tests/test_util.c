#include "util.h"
#include "siphash.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond) do {                       \
    if (!(cond)) {                             \
        failures++;                            \
        fprintf(stderr, "FAIL %s:%d: %s\n",    \
                __FILE__, __LINE__, #cond);    \
    }                                          \
} while (0)

static void test_string_to_ll(void) {
    long long v;

    CHECK(string_to_ll("0", &v) && v == 0);
    CHECK(string_to_ll("123", &v) && v == 123);
    CHECK(string_to_ll("-42", &v) && v == -42);
    CHECK(string_to_ll("+7", &v) && v == 7);

    CHECK(!string_to_ll("", &v));
    CHECK(!string_to_ll("12a", &v));
    CHECK(!string_to_ll("a", &v));
    CHECK(!string_to_ll("--1", &v));
    CHECK(!string_to_ll(" 1", &v));
    CHECK(!string_to_ll("9223372036854775808", &v));  /* LLONG_MAX + 1 */

    CHECK(string_to_ll_n("12345", 3, &v) && v == 123);
    CHECK(!string_to_ll_n("", 0, &v));
    CHECK(!string_to_ll_n("-", 1, &v));
}

static void test_glob(void) {
    CHECK(util_glob_match("*", "anything", 8));
    CHECK(util_glob_match("*", "", 0));
    CHECK(util_glob_match("**", "x", 1));

    CHECK(util_glob_match("user:*", "user:123", 8));
    CHECK(!util_glob_match("user:*", "team:123", 8));

    CHECK(util_glob_match("a?c", "abc", 3));
    CHECK(!util_glob_match("a?c", "ac", 2));
    CHECK(util_glob_match("a?c", "a c", 3));   /* '?' matches any single byte */

    CHECK(util_glob_match("[a-c]at", "bat", 3));
    CHECK(!util_glob_match("[a-c]at", "hat", 3));
    CHECK(util_glob_match("[!a-c]at", "hat", 3));
    CHECK(!util_glob_match("[!a-c]at", "bat", 3));

    CHECK(util_glob_match("*.txt", "report.txt", 10));
    CHECK(!util_glob_match("*.txt", "report.csv", 10));

    CHECK(util_glob_match("file[0-9]", "file7", 5));
    CHECK(!util_glob_match("file[0-9]", "filex", 5));
}

/* SipHash-2-4 known-answer test (published reference vector). */
static void test_siphash(void) {
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t msg[15] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e};
    CHECK(siphash(msg, sizeof(msg), key) == UINT64_C(0xa129ca6149be45e5));

    /* empty message */
    CHECK(siphash(msg, 0, key) != 0);   /* sanity: not the unkeyed zero */
}

int main(void) {
    test_string_to_ll();
    test_glob();
    test_siphash();

    if (failures) {
        fprintf(stderr, "test_util: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_util: all tests passed\n");
    return 0;
}
