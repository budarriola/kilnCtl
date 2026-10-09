// Host-test stub for the update_fetch test: a short build commit that never matches the test manifest's
// full commit, so running_identity() leaves the running commit unknown (a different commit).
#ifndef UPDATE_FETCH_STUB_BUILD_INFO_H
#define UPDATE_FETCH_STUB_BUILD_INFO_H
#define FW_GIT_COMMIT "9999999"
#define FW_GIT_DIRTY 0
#define FW_BUILD_DATE "1970-01-01"
#define FW_BUILD_TIME "00:00:00"
#endif
