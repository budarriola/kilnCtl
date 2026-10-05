// Host tests for App/drivers/update/update_release.c (GITHUB_RELEASE_UPDATE_PLAN.md WP8):
// the bounded GitHub-API / release.json parsers and the asset pick, plus the
// downgrade policy as it is wired into the fetch path (update_policy_decide).
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_policy.h"
#include "../drivers/update/update_release.h"

#define REPO "budarriola/kilnCtl"
#define BASE "https://github.com/budarriola/kilnCtl/releases/download/"
#define SHA_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SHA_P "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define COMMIT "0123456789abcdef0123456789abcdef01234567"
#define MAXAPP 0x400000u

static const char *const API_OK =
    "{\"url\":\"https://api.github.com/x\",\"author\":{\"login\":\"u\",\"name\":\"release.json\"},"
    "\"tag_name\":\"v1.2.3\",\"name\":\"KilnCtrl-v1.2.3.bin\",\"draft\":false,\"prerelease\":false,"
    "\"body\":\"notes \\\"quoted\\\" \\u00e9 \\n\","
    "\"assets\":["
    "{\"name\":\"SHA256SUMS\",\"size\":10,\"browser_download_url\":\"" BASE "v1.2.3/SHA256SUMS\"},"
    "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"uploader\":{\"name\":\"KilnCtrl-v1.2.3.bin\"},\"size\":1234567,"
    "\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"},"
    "{\"name\":\"release.json\",\"size\":900,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}"
    "]}";

static update_rel_err_t api(const char *json, update_release_info_t *out)
{
    return update_release_parse_api(json, strlen(json), REPO, MAXAPP, out);
}

static void test_api_pick(void)
{
    TEST_SECTION("update_release -- API reply parse and asset pick");
    update_release_info_t r;
    TEST_CHECK(api(API_OK, &r) == UPDATE_REL_OK, "well-formed reply parses");
    TEST_CHECK(strcmp(r.tag, "v1.2.3") == 0 && !r.prerelease, "tag and prerelease flag");
    TEST_CHECK(strcmp(r.app_name, "KilnCtrl-v1.2.3.bin") == 0 && r.app_size == 1234567u, "app asset picked");
    TEST_CHECK(strcmp(r.app_url, BASE "v1.2.3/KilnCtrl-v1.2.3.bin") == 0, "app url");
    TEST_CHECK(strcmp(r.manifest_url, BASE "v1.2.3/release.json") == 0 && r.manifest_size == 900u, "manifest asset picked");

    // tag_name after assets: order independence
    const char *rev =
        "{\"assets\":[{\"name\":\"release.json\",\"size\":9,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"},"
        "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":99,\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"}],"
        "\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":true}";
    TEST_CHECK(api(rev, &r) == UPDATE_REL_OK && r.prerelease && r.app_size == 99u, "tag after assets, prerelease reported");

    // prerelease tag
    const char *pre =
        "{\"tag_name\":\"v1.3.0-rc.1\",\"draft\":false,\"prerelease\":true,\"assets\":["
        "{\"name\":\"KilnCtrl-v1.3.0-rc.1.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.3.0-rc.1/KilnCtrl-v1.3.0-rc.1.bin\"},"
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.3.0-rc.1/release.json\"}]}";
    TEST_CHECK(api(pre, &r) == UPDATE_REL_OK && strcmp(r.tag, "v1.3.0-rc.1") == 0, "prerelease tag accepted by the parser");

    // the nested "name":"release.json" under author must not count as an asset
    const char *only_nested =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"author\":{\"name\":\"release.json\"},\"assets\":["
        "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"}]}";
    TEST_CHECK(api(only_nested, &r) == UPDATE_REL_E_NO_MANIFEST, "nested objects are never mistaken for assets");
}

static void test_api_refusals(void)
{
    TEST_SECTION("update_release -- API reply refusals");
    update_release_info_t r;
    TEST_CHECK(api("", &r) == UPDATE_REL_E_ARGS, "empty");
    TEST_CHECK(api("[]", &r) == UPDATE_REL_E_JSON, "array at top level");
    TEST_CHECK(api("{", &r) == UPDATE_REL_E_JSON, "truncated");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\"} x", &r) == UPDATE_REL_E_JSON, "trailing junk");
    TEST_CHECK(api("{\"draft\":false,\"assets\":[]}", &r) == UPDATE_REL_E_NO_TAG, "no tag_name");
    TEST_CHECK(api("{\"tag_name\":123,\"assets\":[]}", &r) == UPDATE_REL_E_BAD_TAG, "tag_name not a string");
    TEST_CHECK(api("{\"tag_name\":\"latest\",\"assets\":[]}", &r) == UPDATE_REL_E_BAD_TAG, "tag not semver");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3/../x\",\"assets\":[]}", &r) == UPDATE_REL_E_BAD_TAG, "path-like tag refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\",\"tag_name\":\"v9.9.9\",\"assets\":[]}", &r) == UPDATE_REL_E_JSON,
                "duplicate tag_name refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\",\"draft\":true,\"assets\":[]}", &r) == UPDATE_REL_E_DRAFT, "draft refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\",\"draft\":\"false\",\"assets\":[]}", &r) == UPDATE_REL_E_JSON,
                "non-boolean draft refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\",\"assets\":[]}", &r) == UPDATE_REL_E_NO_APP, "no assets");

    // size and url problems on the wanted assets
    char buf[2048];
    const char *tmpl =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"assets\":["
        "{\"name\":\"KilnCtrl-v1.2.3.bin\",%s,\"browser_download_url\":\"%s\"},"
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}]}";
    snprintf(buf, sizeof(buf), tmpl, "\"size\":5", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_OK, "template baseline parses");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":5", "https://evil.com/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_URL && r.tag[0] == '\0', "foreign host url refused, out zeroed");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":5", "https://github.com/other/kilnCtl/releases/download/v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_URL, "other repo url refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":5", BASE "v9.9.9/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_URL, "url for another tag refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":0", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "zero size refused");
    snprintf(buf, sizeof(buf), tmpl, "\"x\":1", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "missing size refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":-5", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "negative size refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":5.5", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "fractional size refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":4194305", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "size above the stage cap refused");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":4194304", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_OK && r.app_size == 4194304u, "size exactly at the cap accepted");
    snprintf(buf, sizeof(buf), tmpl, "\"size\":99999999999", BASE "v1.2.3/KilnCtrl-v1.2.3.bin");
    TEST_CHECK(api(buf, &r) == UPDATE_REL_E_BAD_SIZE, "size overflowing uint32 refused");

    // duplicates
    const char *dup =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":["
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"},"
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}]}";
    TEST_CHECK(api(dup, &r) == UPDATE_REL_E_DUP_ASSET, "duplicate release.json refused");

    // a manifest above the 16 KiB fetch buffer
    const char *bigm =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":["
        "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"},"
        "{\"name\":\"release.json\",\"size\":16385,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}]}";
    TEST_CHECK(api(bigm, &r) == UPDATE_REL_E_BAD_SIZE, "oversized release.json refused");

    // a wrong-tag app asset next to nothing else is simply "no app asset"
    const char *wrong =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":["
        "{\"name\":\"KilnCtrl-v1.2.4.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.4/KilnCtrl-v1.2.4.bin\"},"
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}]}";
    TEST_CHECK(api(wrong, &r) == UPDATE_REL_E_NO_APP, "asset named for a different tag is not picked");
    const char *rec =
        "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":["
        "{\"name\":\"KilnRecovery-v1.2.3.bin\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/KilnRecovery-v1.2.3.bin\"},"
        "{\"name\":\"release.json\",\"size\":5,\"browser_download_url\":\"" BASE "v1.2.3/release.json\"}]}";
    TEST_CHECK(api(rec, &r) == UPDATE_REL_E_NO_APP, "the recovery image is never picked as the app");
}

static void test_api_bounds(void)
{
    TEST_SECTION("update_release -- bounded parser: depth, long strings, escapes, NUL safety");
    update_release_info_t r;
    char deep[400];
    size_t n = 0;
    n += (size_t)snprintf(deep + n, sizeof(deep) - n, "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"x\":");
    for (int i = 0; i < 20; i++) {
        deep[n++] = '[';
    }
    for (int i = 0; i < 20; i++) {
        deep[n++] = ']';
    }
    n += (size_t)snprintf(deep + n, sizeof(deep) - n, ",\"assets\":[]}");
    TEST_CHECK(api(deep, &r) == UPDATE_REL_E_JSON, "20-deep nesting refused, not recursed");

    // long body with escapes is skipped without trouble
    static char big[40000];
    size_t m = (size_t)snprintf(big, sizeof(big), "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"body\":\"");
    for (int i = 0; i < 30000; i++) {
        big[m++] = (i % 50 == 0) ? '\\' : 'a';
        if (i % 50 == 0) {
            big[m++] = 'n';
        }
    }
    m += (size_t)snprintf(big + m, sizeof(big) - m,
                          "\",\"assets\":[{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":7,\"browser_download_url\":\"" BASE
                          "v1.2.3/KilnCtrl-v1.2.3.bin\"},{\"name\":\"release.json\",\"size\":7,\"browser_download_url\":\"" BASE
                          "v1.2.3/release.json\"}]}");
    TEST_CHECK(update_release_parse_api(big, m, REPO, MAXAPP, &r) == UPDATE_REL_OK && r.app_size == 7u,
               "30 KB body string with escapes skipped");

    // an over-long url on a wanted asset is refused, not truncated into something that might match
    static char longurl[3000];
    size_t k = (size_t)snprintf(longurl, sizeof(longurl),
                                "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":[{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":7,"
                                "\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin?");
    for (int i = 0; i < 1500; i++) {
        longurl[k++] = 'a';
    }
    k += (size_t)snprintf(longurl + k, sizeof(longurl) - k, "\"}]}");
    TEST_CHECK(update_release_parse_api(longurl, k, REPO, MAXAPP, &r) == UPDATE_REL_E_BAD_URL, "over-long url refused");

    // a name that overflows the 64 byte buffer never matches by truncation
    char longname[600];
    snprintf(longname, sizeof(longname),
             "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":[{\"name\":\"KilnCtrl-v1.2.3.bin%080d\",\"size\":7,"
             "\"browser_download_url\":\"" BASE "v1.2.3/KilnCtrl-v1.2.3.bin\"}]}", 0);
    TEST_CHECK(api(longname, &r) == UPDATE_REL_E_NO_APP, "over-long name is not truncated into a match");

    // bad escapes and raw control characters
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\\q\"}", &r) == UPDATE_REL_E_JSON, "bad escape refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\n\"}", &r) == UPDATE_REL_E_JSON, "raw control char in string refused");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.\\u0033\",\"draft\":false,\"assets\":[]}", &r) == UPDATE_REL_E_NO_APP,
               "\\u escape decoded (v1.2.3), then no assets");
    TEST_CHECK(api("{\"tag_name\":\"v1.2.3\\u0000x\",\"draft\":false,\"assets\":[]}", &r) == UPDATE_REL_E_BAD_TAG,
               "embedded NUL escape never truncates a value");

    // length is honoured: a NUL inside the buffer is JSON garbage, not a terminator
    static const char withnul[] = "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"assets\":[]}\0{\"junk\":1}";
    TEST_CHECK(update_release_parse_api(withnul, sizeof(withnul) - 1, REPO, MAXAPP, &r) == UPDATE_REL_E_JSON,
               "bytes past an embedded NUL are parsed, not ignored");
}

// ---- manifest ----

static const char *const MANIFEST_OK =
    "{\"schema\":1,\"tag\":\"v1.2.3\",\"channel\":\"stable\",\"published\":\"2026-10-05T00:00:00Z\","
    "\"repo\":\"budarriola/kilnCtl\",\"commit\":\"" COMMIT "\",\"dirty\":false,\"build_date\":\"x\","
    "\"compat\":{\"zones_cfg_version\":22,\"kilnlink_version\":16,\"uart_version\":13,\"partitions_sha256\":\"" SHA_P "\"},"
    "\"images\":[{\"name\":\"app\",\"file\":\"KilnCtrl-v1.2.3.bin\",\"size\":1234567,\"sha256\":\"" SHA_A "\","
    "\"includes\":[\"pico_slotA\"]},"
    "{\"name\":\"recovery\",\"file\":\"KilnRecovery-v1.2.3.bin\",\"size\":99,\"sha256\":\"" SHA_A "\",\"apply\":\"jtag_only\"}],"
    "\"notes_url\":\"https://github.com/budarriola/kilnCtl/releases/tag/v1.2.3\"}";

static update_rel_err_t man(const char *json, update_manifest_t *out)
{
    return update_release_parse_manifest(json, strlen(json), REPO, "v1.2.3", 1234567u, out);
}

// Replace the first occurrence of `from` in MANIFEST_OK by `to`.
static const char *mut(char *buf, size_t cap, const char *from, const char *to)
{
    const char *at = strstr(MANIFEST_OK, from);
    if (at == NULL) {
        buf[0] = '\0';
        return buf;
    }
    snprintf(buf, cap, "%.*s%s%s", (int)(at - MANIFEST_OK), MANIFEST_OK, to, at + strlen(from));
    return buf;
}

static void test_manifest(void)
{
    TEST_SECTION("update_release -- release.json parse and refusals");
    update_manifest_t m;
    TEST_CHECK(man(MANIFEST_OK, &m) == UPDATE_REL_OK, "well-formed manifest parses");
    TEST_CHECK(strcmp(m.identity.version, "v1.2.3") == 0 && strcmp(m.identity.commit, COMMIT) == 0 &&
                   strcmp(m.identity.partitions_sha256, SHA_P) == 0,
               "identity strings");
    TEST_CHECK(m.identity.zones_cfg_version == 22 && m.identity.kilnlink_version == 16 && m.identity.uart_version == 13 &&
                   !m.identity.dirty && m.identity.min_updatable_from[0] == '\0',
               "identity numbers, no floor by default");
    TEST_CHECK(m.app_size == 1234567u && strcmp(m.app_sha256, SHA_A) == 0, "app size and sha256");

    char b[3000];
    TEST_CHECK(man(mut(b, sizeof(b), "\"schema\":1", "\"schema\":2"), &m) == UPDATE_REL_E_SCHEMA, "schema 2 refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"schema\":1,", ""), &m) == UPDATE_REL_E_SCHEMA, "missing schema refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"tag\":\"v1.2.3\"", "\"tag\":\"v1.2.4\""), &m) == UPDATE_REL_E_TAG_MISMATCH,
               "tag mismatch refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"repo\":\"budarriola/kilnCtl\"", "\"repo\":\"evil/kilnCtl\""), &m) == UPDATE_REL_E_REPO_MISMATCH,
               "repo mismatch refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"repo\":\"budarriola/kilnCtl\",", ""), &m) == UPDATE_REL_OK, "absent repo tolerated");
    TEST_CHECK(man(mut(b, sizeof(b), COMMIT, "0123456789ABCDEF0123456789abcdef01234567"), &m) == UPDATE_REL_E_BAD_COMMIT,
               "uppercase commit refused");
    TEST_CHECK(man(mut(b, sizeof(b), COMMIT, "0123456"), &m) == UPDATE_REL_E_BAD_COMMIT, "short commit refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"dirty\":false", "\"dirty\":true"), &m) == UPDATE_REL_E_DIRTY, "dirty true refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"dirty\":false,", ""), &m) == UPDATE_REL_E_DIRTY, "missing dirty refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"dirty\":false", "\"dirty\":\"false\""), &m) == UPDATE_REL_E_DIRTY,
               "dirty as a string refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"zones_cfg_version\":22,", ""), &m) == UPDATE_REL_E_BAD_COMPAT, "missing zones version refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"uart_version\":13", "\"uart_version\":0"), &m) == UPDATE_REL_E_BAD_COMPAT, "zero uart version refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"kilnlink_version\":16", "\"kilnlink_version\":\"16\""), &m) == UPDATE_REL_E_BAD_COMPAT,
               "string schema version refused");
    TEST_CHECK(man(mut(b, sizeof(b), SHA_P, "xyz"), &m) == UPDATE_REL_E_BAD_COMPAT, "bad partitions sha refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"partitions_sha256\"", "\"min_updatable_from\":\"v1.0.0\",\"partitions_sha256\""), &m) == UPDATE_REL_OK &&
                   strcmp(m.identity.min_updatable_from, "v1.0.0") == 0,
               "optional min_updatable_from carried");
    TEST_CHECK(man(mut(b, sizeof(b), "\"partitions_sha256\"", "\"min_updatable_from\":\"banana\",\"partitions_sha256\""), &m) == UPDATE_REL_E_BAD_COMPAT,
               "unparsable min_updatable_from refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"name\":\"app\"", "\"name\":\"apx\""), &m) == UPDATE_REL_E_NO_APP_IMAGE, "no app image refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"file\":\"KilnCtrl-v1.2.3.bin\"", "\"file\":\"KilnCtrl-v9.bin\""), &m) == UPDATE_REL_E_NO_APP_IMAGE,
               "app file name must match the tag");
    TEST_CHECK(man(mut(b, sizeof(b), "\"size\":1234567", "\"size\":1234568"), &m) == UPDATE_REL_E_SIZE_MISMATCH,
               "manifest size must equal the API asset size");
    TEST_CHECK(man(mut(b, sizeof(b), "\"size\":1234567", "\"size\":0"), &m) == UPDATE_REL_E_NO_APP_IMAGE, "zero app size refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"sha256\":\"" SHA_A "\",\"includes\"", "\"sha256\":\"AAAA\",\"includes\""), &m) == UPDATE_REL_E_NO_APP_IMAGE,
               "malformed app sha256 refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"includes\":[\"pico_slotA\"]}", "\"includes\":[\"pico_slotA\"]},{\"name\":\"app\",\"file\":\"KilnCtrl-v1.2.3.bin\",\"size\":1234567,\"sha256\":\"" SHA_A "\"}"), &m) == UPDATE_REL_E_NO_APP_IMAGE,
               "two app images refused");
    TEST_CHECK(man(mut(b, sizeof(b), "\"commit\"", "\"commit\":\"" COMMIT "\",\"commit\""), &m) == UPDATE_REL_E_JSON,
               "duplicate commit key refused");
    TEST_CHECK(man("{\"schema\":1", &m) == UPDATE_REL_E_JSON, "truncated manifest refused");
    TEST_CHECK(update_release_parse_manifest(MANIFEST_OK, strlen(MANIFEST_OK), REPO, "bad", 1, &m) == UPDATE_REL_E_ARGS,
               "bad tag argument");
}

// ---- version policy as wired into the fetch path ----

static void fill_running(update_identity_t *r, const char *ver)
{
    memset(r, 0, sizeof(*r));
    snprintf(r->version, sizeof(r->version), "%s", ver);
    snprintf(r->commit, sizeof(r->commit), "%s", COMMIT);
    snprintf(r->partitions_sha256, sizeof(r->partitions_sha256), "%s", SHA_P);
    r->zones_cfg_version = 22;
    r->kilnlink_version = 16;
    r->uart_version = 13;
}

static void test_policy_from_manifest(void)
{
    TEST_SECTION("update_release -- candidate identity from a manifest feeds update_policy_decide");
    update_manifest_t m;
    TEST_CHECK(man(MANIFEST_OK, &m) == UPDATE_REL_OK, "manifest parses");
    update_identity_t run;
    update_policy_flags_t none = {0};
    update_policy_flags_t dg = {0};
    dg.allow_downgrade = true;

    fill_running(&run, "v1.2.2");
    update_decision_t d = update_policy_decide(&run, &m.identity, &none);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_UPGRADE && d.allowed, "newer release over v1.2.2: upgrade");

    fill_running(&run, "v1.3.0");
    d = update_policy_decide(&run, &m.identity, &none);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_DOWNGRADE && !d.allowed, "older release than running v1.3.0: refused");
    d = update_policy_decide(&run, &m.identity, &dg);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_ALLOW_DOWNGRADE && d.allowed && d.needs_typed_confirm,
               "admin override allows it with a typed confirm");

    fill_running(&run, "v1.2.3");
    d = update_policy_decide(&run, &m.identity, &none);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_UP_TO_DATE && !d.allowed, "same version and commit: up to date");

    fill_running(&run, "v1.2.2");
    snprintf(run.partitions_sha256, sizeof(run.partitions_sha256), "%s", SHA_A);
    d = update_policy_decide(&run, &m.identity, &dg);
    TEST_CHECK(d.verdict == UPDATE_VERDICT_REFUSE_PARTITIONS, "partition map mismatch never overridable");
}

void run_test_update_release(void)
{
    test_api_pick();
    test_api_refusals();
    test_api_bounds();
    test_manifest();
    test_policy_from_manifest();
}
