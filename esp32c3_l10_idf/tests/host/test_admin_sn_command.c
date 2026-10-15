#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if __has_include("admin_sn_command.h")
#include "admin_sn_command.h"
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static admin_sn_result_t feed(admin_sn_input_t *s, const char *line,
                              const char *password, char sn[17])
{
    admin_sn_result_t result = ADMIN_SN_NONE;
    while (*line) {
        admin_sn_result_t r = admin_sn_input_byte(s, (unsigned char)*line++, password, sn);
        if (r != ADMIN_SN_NONE) result = r;
    }
    return result;
}
int main(void)
{
    admin_sn_input_t s = {0};
    char sn[17] = {0};
    CHECK(feed(&s, "set_sn test-password H1CNA0013500001O\r\n", "test-password", sn) == ADMIN_SN_VALID);
    CHECK(strcmp(sn, "H1CNA0013500001O") == 0);
    for (size_t i = 0; i < sizeof(s.line); i++) CHECK(s.line[i] == 0);
    CHECK(feed(&s, "set_sn wrong ABC\n", "test-password", sn) == ADMIN_SN_AUTH_FAILED);
    CHECK(feed(&s, "set_sn test-password ABC\n", "", sn) == ADMIN_SN_AUTH_FAILED);
    CHECK(feed(&s, "set_sn test-password ABCDEF01234567890\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password A/B\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password A+\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password A extra\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "other test-password A\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password ABCX\b\n", "test-password", sn) == ADMIN_SN_VALID);
    CHECK(strcmp(sn, "ABC") == 0);
    CHECK(feed(&s, "set_sn test-password A\001B\n", "test-password", sn) == ADMIN_SN_INVALID_ARGS);
    CHECK(feed(&s, "set_sn test-password A", "test-password", sn) == ADMIN_SN_NONE);
    CHECK(feed(&s, "\n", "test-password", sn) == ADMIN_SN_VALID);
    for (size_t i = 0; i < sizeof(s.line) + 10; i++)
        CHECK(admin_sn_input_byte(&s, 'X', "test-password", sn) == ADMIN_SN_NONE);
    CHECK(feed(&s, "\bset_sn test-password A\r\n", "test-password", sn) == ADMIN_SN_TOO_LONG);
    CHECK(feed(&s, "set_sn test-password a1\n", "test-password", sn) == ADMIN_SN_VALID);
    CHECK(strcmp(sn, "a1") == 0);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
#else
int main(void) { fputs("FAIL: serial admin SN parser is missing\n", stderr); return EXIT_FAILURE; }
#endif
