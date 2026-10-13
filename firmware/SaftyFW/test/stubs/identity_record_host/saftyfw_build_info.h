#ifndef SAFTYFW_TEST_STUB_IDENTITY_RECORD_BUILD_INFO_H
#define SAFTYFW_TEST_STUB_IDENTITY_RECORD_BUILD_INFO_H
/* Fixed identity for test_saftyfw_image_identity_record.c: a 12-character
 * commit and dirty=1, so the test can assert exact bytes. */
#define SAFTYFW_GIT_COMMIT "0123456789ab"
#define SAFTYFW_GIT_DIRTY 7 /* non-0/1 on purpose: the record must normalise it to 1 */
#endif
