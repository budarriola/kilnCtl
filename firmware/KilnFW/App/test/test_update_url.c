// Host tests for App/drivers/update/update_url.c (GITHUB_RELEASE_UPDATE_PLAN.md WP8).
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_url.h"

static void test_allowlist(void)
{
    TEST_SECTION("update_url -- host allowlist is exact, never a suffix or substring match");
    static const char *const good[] = {
        "https://api.github.com/repos/a/b/releases/latest",
        "https://github.com/a/b/releases/download/v1.0.0/x.bin",
        "https://objects.githubusercontent.com/github-production-release-asset/1?X=Y&Z=W",
        "https://release-assets.githubusercontent.com/github-production-release-asset/2?sp=r",
        "https://GITHUB.com/a",
        "HTTPS://Api.GitHub.com:443/x",
        "https://github.com",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_CHECK(update_url_check(good[i], NULL, 0) == UPDATE_URL_OK, good[i]);
    }
    static const char *const bad_host[] = {
        "https://evil.com/",
        "https://github.com.evil.com/a",
        "https://evilgithub.com/a",
        "https://notgithub.com/a",
        "https://sub.github.com/a",
        "https://raw.githubusercontent.com/a/b/c",
        "https://githubusercontent.com/a",
        "https://evil.githubusercontent.com/a",
        "https://objects.githubusercontent.com.evil.com/a",
        "https://api.github.com.evil.com/",
        "https://github.com.attacker.example/https://github.com/",
    };
    for (size_t i = 0; i < sizeof(bad_host) / sizeof(bad_host[0]); i++) {
        TEST_CHECK(update_url_check(bad_host[i], NULL, 0) == UPDATE_URL_E_HOST, bad_host[i]);
    }
    char h[UPDATE_HOST_MAX];
    TEST_CHECK(update_url_check("https://API.GitHub.com/x", h, sizeof(h)) == UPDATE_URL_OK &&
                   strcmp(h, "api.github.com") == 0,
               "host is returned lowercased");
    TEST_CHECK(update_url_check("https://api.github.com/x", h, 5) == UPDATE_URL_E_MALFORMED,
               "host buffer too small refused");
}

static void test_malformed(void)
{
    TEST_SECTION("update_url -- scheme, userinfo, port and character rules");
    TEST_CHECK(update_url_check(NULL, NULL, 0) == UPDATE_URL_E_EMPTY, "NULL");
    TEST_CHECK(update_url_check("", NULL, 0) == UPDATE_URL_E_EMPTY, "empty");
    TEST_CHECK(update_url_check("http://github.com/a", NULL, 0) == UPDATE_URL_E_SCHEME, "http refused");
    TEST_CHECK(update_url_check("ftp://github.com/a", NULL, 0) == UPDATE_URL_E_SCHEME, "ftp refused");
    TEST_CHECK(update_url_check("//github.com/a", NULL, 0) == UPDATE_URL_E_SCHEME, "scheme-relative refused");
    TEST_CHECK(update_url_check("/a/b", NULL, 0) == UPDATE_URL_E_SCHEME, "relative path refused");
    TEST_CHECK(update_url_check("https://", NULL, 0) == UPDATE_URL_E_MALFORMED, "no host");
    TEST_CHECK(update_url_check("https:///a", NULL, 0) == UPDATE_URL_E_MALFORMED, "empty authority");
    TEST_CHECK(update_url_check("https://user@github.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "userinfo refused");
    TEST_CHECK(update_url_check("https://github.com@evil.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED,
               "userinfo trick (allowed name before @) refused");
    TEST_CHECK(update_url_check("https://evil.com@github.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED,
               "userinfo refused (reverse)");
    TEST_CHECK(update_url_check("https://github.com:8443/a", NULL, 0) == UPDATE_URL_E_MALFORMED,
               "non-443 port refused");
    TEST_CHECK(update_url_check("https://github.com:/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "empty port refused");
    TEST_CHECK(update_url_check("https://github.com:443/a", NULL, 0) == UPDATE_URL_OK, ":443 accepted");
    TEST_CHECK(update_url_check("https://[::1]/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "IPv6 literal refused");
    TEST_CHECK(update_url_check("https://127.0.0.1/a", NULL, 0) == UPDATE_URL_E_HOST,
               "IP literal not on allowlist");
    TEST_CHECK(update_url_check("https://github.com\\@evil.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED,
               "backslash refused");
    TEST_CHECK(update_url_check("https://git hub.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "space refused");
    TEST_CHECK(update_url_check("https://github.com/a\r\nHost: evil", NULL, 0) == UPDATE_URL_E_MALFORMED,
               "CRLF injection refused");
    TEST_CHECK(update_url_check("https://github..com/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "empty label refused");
    TEST_CHECK(update_url_check("https://.github.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "leading dot refused");
    TEST_CHECK(update_url_check("https://github.com./a", NULL, 0) == UPDATE_URL_E_MALFORMED, "trailing dot refused");
    TEST_CHECK(update_url_check("https://git_hub.com/a", NULL, 0) == UPDATE_URL_E_MALFORMED, "underscore refused");
    TEST_CHECK(update_url_check("https://github.com?x=https://evil.com", NULL, 0) == UPDATE_URL_OK,
               "query after bare host is fine");

    char longurl[UPDATE_URL_MAX + 8];
    memset(longurl, 'a', sizeof(longurl));
    memcpy(longurl, "https://github.com/", 19);
    longurl[UPDATE_URL_MAX] = '\0';
    TEST_CHECK(update_url_check(longurl, NULL, 0) == UPDATE_URL_E_TOO_LONG, "URL of UPDATE_URL_MAX bytes refused");
    longurl[UPDATE_URL_MAX - 1] = '\0';
    TEST_CHECK(update_url_check(longurl, NULL, 0) == UPDATE_URL_OK, "one byte shorter accepted");

    const char *path = NULL;
    char h[UPDATE_HOST_MAX];
    TEST_CHECK(update_url_parse("https://github.com/a/b?c=d", h, sizeof(h), &path) == UPDATE_URL_OK &&
                   strcmp(path, "/a/b?c=d") == 0,
               "path includes query");
    TEST_CHECK(update_url_parse("https://github.com", h, sizeof(h), &path) == UPDATE_URL_OK &&
                   strcmp(path, "/") == 0,
               "absent path is /");
}

static void test_redirects(void)
{
    TEST_SECTION("update_url -- redirect budget and per-hop checks");
    const char *ok = "https://objects.githubusercontent.com/x";
    TEST_CHECK(update_redirect_check(0, ok) == UPDATE_URL_OK, "first hop ok");
    TEST_CHECK(update_redirect_check(1, ok) == UPDATE_URL_OK, "second hop ok");
    TEST_CHECK(update_redirect_check(2, ok) == UPDATE_URL_OK, "third hop ok");
    TEST_CHECK(update_redirect_check(3, ok) == UPDATE_URL_E_TOO_MANY_HOPS, "fourth hop refused");
    TEST_CHECK(update_redirect_check(99, ok) == UPDATE_URL_E_TOO_MANY_HOPS, "far past the cap refused");
    TEST_CHECK(update_redirect_check(0, "https://evil.com/x") == UPDATE_URL_E_HOST,
               "redirect off the allowlist refused");
    TEST_CHECK(update_redirect_check(0, "http://github.com/x") == UPDATE_URL_E_SCHEME,
               "redirect downgrade to http refused");
    TEST_CHECK(update_redirect_check(0, "/relative") == UPDATE_URL_E_SCHEME, "relative Location refused");
    TEST_CHECK(update_redirect_check(0, NULL) == UPDATE_URL_E_EMPTY, "missing Location refused");
}

static void test_repo_and_builders(void)
{
    TEST_SECTION("update_url -- repo validation, API URL builder, tag shape");
    TEST_CHECK(update_repo_valid("budarriola/kilnCtl"), "default repo valid");
    TEST_CHECK(update_repo_valid("a/b"), "minimal repo");
    TEST_CHECK(update_repo_valid("a.b_c-d/e.f_g-h"), "punctuation inside parts");
    static const char *const bad[] = {
        "", "a", "a/", "/b", "a/b/c", "a b/c", "a/b c", "../b", "a/..", "a/b..c", "-a/b", "a-/b", "a/.",
        "a/b?x=1", "a/b#f", "a/b%2f", "a\\b", "a/b\n", "\xc3\xa9/b",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_CHECK(!update_repo_valid(bad[i]), bad[i]);
    }
    TEST_CHECK(!update_repo_valid(NULL), "NULL repo");
    char buf[200];
    memset(buf, 'a', 40);
    memcpy(buf + 40, "/b", 3);
    TEST_CHECK(!update_repo_valid(buf), "owner of 40 chars refused");
    memset(buf, 'a', 39);
    memcpy(buf + 39, "/b", 3);
    TEST_CHECK(update_repo_valid(buf), "owner of 39 chars accepted");
    memcpy(buf, "a/", 2);
    memset(buf + 2, 'b', 101);
    buf[103] = '\0';
    TEST_CHECK(!update_repo_valid(buf), "name of 101 chars refused");
    buf[102] = '\0';
    TEST_CHECK(update_repo_valid(buf), "name of 100 chars accepted");

    char url[UPDATE_URL_MAX];
    TEST_CHECK(update_url_build_latest("budarriola/kilnCtl", url, sizeof(url)) &&
                   strcmp(url, "https://api.github.com/repos/budarriola/kilnCtl/releases/latest") == 0,
               "latest URL");
    TEST_CHECK(update_url_check(url, NULL, 0) == UPDATE_URL_OK, "built URL passes its own check");
    TEST_CHECK(!update_url_build_latest("bad repo", url, sizeof(url)) && url[0] == '\0',
               "bad repo refused, out emptied");
    TEST_CHECK(!update_url_build_latest("a/b", url, 20) && url[0] == '\0', "small buffer refused");

    TEST_CHECK(update_tag_valid("v1.0.0") && update_tag_valid("v10.20.30-rc.1"), "valid tags");
    static const char *const badtag[] = {"",         "1.0.0",       "v1.0",      "v1.0.0+build", "v1.0.0/x",
                                         "v1.0.0 ", "V1.0.0",      "v1.0.0-a%", "v01.0.0",      "vv1.0.0",
                                         "v1.0.0-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    for (size_t i = 0; i < sizeof(badtag) / sizeof(badtag[0]); i++) {
        TEST_CHECK(!update_tag_valid(badtag[i]), badtag[i]);
    }
    TEST_CHECK(!update_tag_valid(NULL), "NULL tag");
}

static void test_asset_pin(void)
{
    TEST_SECTION("update_url -- asset URL pinned to repo, tag and name");
    const char *r = "budarriola/kilnCtl";
    const char *good = "https://github.com/budarriola/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin";
    TEST_CHECK(update_asset_url_matches(good, r, "v1.2.3", "KilnCtrl-v1.2.3.bin"), "exact URL accepted");
    TEST_CHECK(update_asset_url_matches("https://github.com/BUDARRIOLA/kilnctl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin",
                                        r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "repo compared case-insensitively");
    TEST_CHECK(!update_asset_url_matches("https://github.com/other/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "other repo refused");
    TEST_CHECK(!update_asset_url_matches("https://github.com/budarriola/kilnCtl/releases/download/v1.2.4/KilnCtrl-v1.2.3.bin",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "other tag refused");
    TEST_CHECK(!update_asset_url_matches("https://github.com/budarriola/kilnCtl/releases/download/v1.2.3/Other.bin", r,
                                         "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "other asset name refused");
    TEST_CHECK(!update_asset_url_matches("https://github.com/budarriola/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin?x=1",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "query refused");
    TEST_CHECK(!update_asset_url_matches("https://github.com/budarriola/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin/",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "trailing slash refused");
    TEST_CHECK(!update_asset_url_matches("https://api.github.com/budarriola/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "other allowed host refused (asset URLs are github.com only)");
    TEST_CHECK(!update_asset_url_matches("http://github.com/budarriola/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin",
                                         r, "v1.2.3", "KilnCtrl-v1.2.3.bin"),
               "http refused");
    TEST_CHECK(!update_asset_url_matches(good, "bad repo", "v1.2.3", "KilnCtrl-v1.2.3.bin"), "bad repo refused");
    TEST_CHECK(!update_asset_url_matches(good, r, "1.2.3", "KilnCtrl-v1.2.3.bin"), "bad tag refused");
    TEST_CHECK(!update_asset_url_matches(NULL, r, "v1.2.3", "x"), "NULL url refused");
}

void run_test_update_url(void)
{
    test_allowlist();
    test_malformed();
    test_redirects();
    test_repo_and_builders();
    test_asset_pin();
}
