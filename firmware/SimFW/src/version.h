// version.h -- SimFW's own firmware version (major.minor.patch), separate
// from BENCHPROTO_PROTOCOL_VERSION (firmware/CommonFW/include/benchproto/
// benchproto_version.h), which is the *wire protocol's* version, not this
// firmware image's. cmd_task's SYS GET_VERSION/GET_CAPS handlers
// (src/tasks/cmd_task.c) report both numbers separately, same split
// KilnFW/SaftyFW draw between their own firmware version and kilnlink's
// KILNLINK_PROTOCOL_VERSION -- see that pair's own headers for the same
// reasoning, applied here to benchproto instead.
//
// Bump PATCH for any change to this firmware; bump MINOR for a new command
// or capability GET_CAPS should start reporting; bump MAJOR for a breaking
// change to what a peer can assume about this firmware's behavior. A human
// judgement call, deliberately not automatic -- same policy
// benchproto_version.h documents for BENCHPROTO_PROTOCOL_VERSION.
#ifndef SIMFW_VERSION_H
#define SIMFW_VERSION_H

#define SIMFW_FW_VERSION_MAJOR 0u
#define SIMFW_FW_VERSION_MINOR 1u
#define SIMFW_FW_VERSION_PATCH 0u

#endif // SIMFW_VERSION_H
