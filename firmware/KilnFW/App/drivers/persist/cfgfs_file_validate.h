// cfgfs_file_validate -- content check for POST /api/cfgfs/file (audit M7,
// docs/audits/HTTP_INPUT_PARSING_AUDIT_2026-10-09.md).
//
// A body is accepted only if the firmware's own loader for that file name
// would accept it. Names with no validator are refused unless the caller
// passed raw=1 (the explicit restore escape hatch).
#ifndef CFGFS_FILE_VALIDATE_H
#define CFGFS_FILE_VALIDATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CFGFS_FILE_CHECK_OK = 0,     /* write may proceed */
    CFGFS_FILE_CHECK_INVALID,    /* body fails the loader's validation: 400, write nothing */
    CFGFS_FILE_CHECK_NO_VALIDATOR, /* no validator for this name and raw=1 absent: 400 */
    CFGFS_FILE_CHECK_OOM,        /* scratch allocation failed: 500, write nothing */
} cfgfs_file_check_t;

/* *reason (may be NULL) receives a short static string on every non-OK result. */
cfgfs_file_check_t cfgfs_file_check_write(const char *name, bool raw, const void *body, size_t len,
                                          const char **reason);

#ifdef __cplusplus
}
#endif

#endif
