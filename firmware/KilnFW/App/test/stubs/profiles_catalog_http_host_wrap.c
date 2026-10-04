/* Host-build wrapper for drivers/http/profiles_catalog_http.c, used only by the
 * store-link host test (test_profile_executor_store_link.c).
 *
 * That file embeds its HTML page via GCC's asm("_binary_...") symbol-naming
 * extension, which MSVC cannot parse and which cannot be defined away from the
 * command line (function-like /D macros are unsupported). test_profiles_http.c
 * #defines asm(x) away ahead of its own #include; this wrapper does the same
 * for a separately compiled object, and supplies the two page symbols. */
#define asm(x)
#include "../../drivers/http/profiles_catalog_http.c"
#undef asm

const uint8_t profiles_page_html_gz_start[1] = { 0 };
const uint8_t profiles_page_html_gz_end[1] = { 0 };
