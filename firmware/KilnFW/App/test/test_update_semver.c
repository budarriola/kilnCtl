// Host tests for App/drivers/update/update_semver.c (docs/GITHUB_RELEASE_UPDATE_PLAN.md D3).
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_semver.h"

static int cmp_str(const char *a, const char *b)
{
    update_semver_t x, y;
    if (!update_semver_parse(a, &x) || !update_semver_parse(b, &y)) {
        return 99;
    }
    return update_semver_compare(&x, &y);
}

static void test_parse_valid(void)
{
    TEST_SECTION("update_semver -- valid parses");
    update_semver_t v;
    TEST_CHECK(update_semver_parse("v1.0.0", &v), "v1.0.0 parses");
    TEST_CHECK(v.major == 1 && v.minor == 0 && v.patch == 0 && !v.has_pre, "v1.0.0 fields");
    TEST_CHECK(update_semver_parse("1.2.3", &v) && v.major == 1 && v.minor == 2 && v.patch == 3,
               "no leading v accepted");
    TEST_CHECK(update_semver_parse("V10.20.30", &v) && v.major == 10 && v.minor == 20 && v.patch == 30,
               "capital V accepted");
    TEST_CHECK(update_semver_parse("0.0.0", &v) && v.major == 0, "0.0.0 parses");
    TEST_CHECK(update_semver_parse("v1.0.0-rc.1", &v) && v.has_pre && strcmp(v.pre, "rc.1") == 0,
               "prerelease captured");
    TEST_CHECK(update_semver_parse("1.0.0+abc.123", &v) && !v.has_pre, "build metadata accepted, not a prerelease");
    TEST_CHECK(update_semver_parse("1.0.0-beta+exp.sha.5114f85", &v) && strcmp(v.pre, "beta") == 0,
               "prerelease stops at +");
    TEST_CHECK(update_semver_parse("1.0.0-0.3.7", &v) && strcmp(v.pre, "0.3.7") == 0, "numeric prerelease ids");
    TEST_CHECK(update_semver_parse("1.0.0-x-y-z.--", &v), "hyphens inside identifiers accepted");
    TEST_CHECK(update_semver_parse("999999999.999999999.999999999", &v) && v.major == 999999999u,
               "nine-digit maximum accepted");
}

static void test_parse_invalid(void)
{
    TEST_SECTION("update_semver -- malformed input is rejected and zeroes the output");
    static const char *const bad[] = {
        "", "v", "1", "1.0", "1.0.", "1..0", ".1.0", "1.0.0.0", "v1.0", "01.0.0", "1.02.0", "1.0.03",
        "1.0.0-", "1.0.0-.", "1.0.0-rc..1", "1.0.0-rc.", "1.0.0-01", "1.0.0-rc.01", "1.0.0+", "1.0.0+a..b",
        "1.0.0 ", " 1.0.0", "1.0.0\n", "a.b.c", "1.0.0-rc_1", "1.0.0-é", "vv1.0.0", "-1.0.0", "1.-1.0",
        "1000000000.0.0", "1.0.0-a?b", "1.0.0+a b",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        update_semver_t v;
        memset(&v, 0x5A, sizeof(v));
        TEST_CHECK(!update_semver_parse(bad[i], &v), bad[i]);
        TEST_CHECK(v.major == 0 && v.minor == 0 && v.patch == 0 && !v.has_pre && v.pre[0] == '\0',
                   "output zeroed after a failed parse");
    }
    update_semver_t v;
    TEST_CHECK(!update_semver_parse(NULL, &v), "NULL string rejected");
    TEST_CHECK(!update_semver_parse("1.0.0", NULL), "NULL out rejected");
    TEST_CHECK(!update_semver_parse_n("1.0.0", 0, &v), "zero length rejected");

    // Prerelease length limit: 31 ok, 32 rejected.
    char buf[96];
    strcpy(buf, "1.0.0-");
    memset(buf + 6, 'a', 31);
    buf[6 + 31] = '\0';
    TEST_CHECK(update_semver_parse(buf, &v) && strlen(v.pre) == 31, "31-char prerelease accepted");
    buf[6 + 31] = 'a';
    buf[6 + 32] = '\0';
    TEST_CHECK(!update_semver_parse(buf, &v), "32-char prerelease rejected");

    // Over 64 bytes total is rejected without reading past the bound.
    char longbuf[200];
    memset(longbuf, '1', sizeof(longbuf));
    longbuf[sizeof(longbuf) - 1] = '\0';
    TEST_CHECK(!update_semver_parse(longbuf, &v), "over-long input rejected");
}

static void test_parse_n_bounded(void)
{
    TEST_SECTION("update_semver -- bounded parse honours the length, not a NUL");
    update_semver_t v;
    const char raw[] = "1.2.3garbage";
    TEST_CHECK(update_semver_parse_n(raw, 5, &v) && v.patch == 3, "first 5 bytes parse as 1.2.3");
    TEST_CHECK(!update_semver_parse_n(raw, 6, &v), "including trailing junk byte fails");
}

static void test_compare_core(void)
{
    TEST_SECTION("update_semver -- major/minor/patch ordering");
    TEST_CHECK(cmp_str("1.0.0", "1.0.0") == 0, "equal");
    TEST_CHECK(cmp_str("v1.0.0", "1.0.0") == 0, "leading v irrelevant");
    TEST_CHECK(cmp_str("1.0.1", "1.0.0") == 1, "patch greater");
    TEST_CHECK(cmp_str("1.0.0", "1.0.1") == -1, "patch less");
    TEST_CHECK(cmp_str("1.1.0", "1.0.9") == 1, "minor beats patch");
    TEST_CHECK(cmp_str("2.0.0", "1.99.99") == 1, "major beats minor");
    TEST_CHECK(cmp_str("1.10.0", "1.9.0") == 1, "numeric not lexical (10 > 9)");
    TEST_CHECK(cmp_str("0.9.0", "1.0.0") == -1, "0.9.0 < 1.0.0");
    TEST_CHECK(cmp_str("1.0.0+a", "1.0.0+b") == 0, "build metadata ignored");
}

static void test_compare_prerelease(void)
{
    TEST_SECTION("update_semver -- prerelease ordering (semver 2.0.0 section 11 chain)");
    // The canonical ascending chain from the semver spec.
    static const char *const chain[] = {
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1", "1.0.0",
    };
    size_t n = sizeof(chain) / sizeof(chain[0]);
    for (size_t i = 0; i + 1 < n; i++) {
        TEST_CHECK(cmp_str(chain[i], chain[i + 1]) == -1, chain[i]);
        TEST_CHECK(cmp_str(chain[i + 1], chain[i]) == 1, chain[i + 1]);
    }
    TEST_CHECK(cmp_str("1.0.0-rc.1", "1.0.0-rc.1") == 0, "equal prereleases");
    TEST_CHECK(cmp_str("1.0.0-1", "1.0.0-alpha") == -1, "numeric id < alphanumeric id");
    TEST_CHECK(cmp_str("1.0.0-2", "1.0.0-10") == -1, "numeric ids compare numerically");
    TEST_CHECK(cmp_str("1.0.0-alpha", "1.0.0-Alpha") == 1, "ASCII order: lowercase after uppercase");
    TEST_CHECK(cmp_str("1.0.1-rc.1", "1.0.0") == 1, "prerelease of a higher patch beats a lower release");
    TEST_CHECK(cmp_str("1.0.0-rc.1", "0.9.9") == 1, "prerelease of 1.0.0 beats 0.9.9");
    TEST_CHECK(cmp_str("1.0.0-123456789", "1.0.0-23456789") == 1, "long numeric id larger than shorter one");
}

static void test_format(void)
{
    TEST_SECTION("update_semver -- format");
    update_semver_t v;
    char buf[64];
    TEST_CHECK(update_semver_parse("v1.2.3-rc.4+meta", &v), "parse");
    TEST_CHECK(update_semver_format(&v, buf, sizeof(buf)) == 10 && strcmp(buf, "1.2.3-rc.4") == 0,
               "format drops v and build metadata");
    TEST_CHECK(update_semver_parse("1.2.3", &v) && update_semver_format(&v, buf, sizeof(buf)) == 5 &&
                   strcmp(buf, "1.2.3") == 0,
               "release format");
    TEST_CHECK(update_semver_format(&v, buf, 5) == 0 && buf[0] == '\0', "too small buffer returns 0, empty string");
    TEST_CHECK(update_semver_format(&v, buf, 6) == 5, "exact-fit buffer ok");
    TEST_CHECK(update_semver_format(NULL, buf, sizeof(buf)) == 0, "NULL value rejected");
    TEST_CHECK(update_semver_format(&v, NULL, 10) == 0, "NULL buffer rejected");
}

void run_test_update_semver(void)
{
    test_parse_valid();
    test_parse_invalid();
    test_parse_n_bounded();
    test_compare_core();
    test_compare_prerelease();
    test_format();
}
