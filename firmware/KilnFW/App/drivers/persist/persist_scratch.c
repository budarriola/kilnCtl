#include "persist_scratch.h"

/* FX-05: one shared, atomic PSRAM-fallback counter for every translation unit
 * (the old function-local static in the header was per-TU, non-atomic, unread). */
static volatile uint32_t s_persist_scratch_fallbacks;

uint32_t persist_scratch_note_fallback(void)
{
#if defined(__GNUC__)
    return __atomic_add_fetch(&s_persist_scratch_fallbacks, 1u, __ATOMIC_RELAXED);
#else
    return ++s_persist_scratch_fallbacks; /* host (MSVC) tests are single-threaded */
#endif
}

uint32_t persist_scratch_fallback_count(void)
{
#if defined(__GNUC__)
    return __atomic_load_n(&s_persist_scratch_fallbacks, __ATOMIC_RELAXED);
#else
    return s_persist_scratch_fallbacks;
#endif
}
