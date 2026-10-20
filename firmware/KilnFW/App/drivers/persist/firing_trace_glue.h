#ifndef FIRING_TRACE_GLUE_H
#define FIRING_TRACE_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the cfg_fs / flash-worker IO table and restores the trace index. Call after cfg_fs mounts,
 * before profile_executor_start(). No-op when the cfg partition is unavailable. */
void firing_trace_glue_init(void);

#ifdef __cplusplus
}
#endif

#endif /* FIRING_TRACE_GLUE_H */
