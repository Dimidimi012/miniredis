#include "resp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do {                       \
    if (!(cond)) {                             \
        failures++;                            \
        fprintf(stderr, "FAIL %s:%d: %s\n",    \
                __FILE__, __LINE__, #cond);    \
    }                                          \
} while (0)

static void assert_command(const char *buf, int expected_argc, const char **expected) {
    command c;
    command_init(&c);
    int consumed = resp_parse_command(buf, strlen(buf), &c);
    CHECK(consumed == (int)strlen(buf));
    if (consumed > 0) {
        CHECK(c.argc == expected_argc);
        for (int i = 0; i < expected_argc && i < c.argc; i++) {
            CHECK(strcmp(c.argv[i], expected[i]) == 0);
        }
    }
    command_free(&c);
}

int main(void) {
    {
        const char *argv[] = {"SET", "key", "value"};
        assert_command("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n", 3, argv);
    }
    {
        const char *argv[] = {"PING"};
        assert_command("*1\r\n$4\r\nPING\r\n", 1, argv);
    }
    {
        const char *argv[] = {"ECHO", "hello world"};
        assert_command("*2\r\n$4\r\nECHO\r\n$11\r\nhello world\r\n", 2, argv);
    }
    {
        const char *argv[] = {NULL};
        assert_command("*0\r\n", 0, argv);
    }
    {
        /* incomplete frame -> 0 */
        const char *buf = "*2\r\n$3\r\nSET\r\n";
        command c;
        command_init(&c);
        CHECK(resp_parse_command(buf, strlen(buf), &c) == 0);
        command_free(&c);
    }
    {
        /* wrong top-level type -> protocol error */
        const char *buf = ":5\r\n";
        command c;
        command_init(&c);
        CHECK(resp_parse_command(buf, strlen(buf), &c) == -1);
        command_free(&c);
    }
    {
        /* bad bulk-string terminator -> protocol error */
        const char *buf = "*1\r\n$3\r\nSETxx";
        command c;
        command_init(&c);
        CHECK(resp_parse_command(buf, strlen(buf), &c) == -1);
        command_free(&c);
    }

    if (failures) {
        fprintf(stderr, "test_resp: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_resp: all tests passed\n");
    return 0;
}
