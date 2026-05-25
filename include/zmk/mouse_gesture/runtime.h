/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Phase 5 addition (shakushakupanda fork): expose a runtime-replacement
 * API for the gesture pattern set. External modules construct a
 * `struct gesture_pattern[]` array and pass it to
 * zmk_mouse_gesture_runtime_set(); the driver rebuilds its trie from
 * the new set. Caller must keep the array (and the pattern[]/bindings[]
 * sub-arrays) alive for the lifetime of the trie (until next call or
 * program end).
 */

#ifndef ZMK_MOUSE_GESTURE_RUNTIME_H_
#define ZMK_MOUSE_GESTURE_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>
#include <drivers/behavior.h>  /* for struct zmk_behavior_binding */

#ifdef __cplusplus
extern "C" {
#endif

/* Same shape as the previously file-local struct in
 * src/input_processors/input_processor_mouse_gesture.c. Relocated here
 * so external modules can construct one. */
struct gesture_pattern {
    size_t                              bindings_len;
    const struct zmk_behavior_binding  *bindings;
    size_t                              pattern_len;
    uint32_t                            wait_ms;
    uint32_t                            tap_ms;
    const uint8_t                      *pattern;
};

/*
 * Replace the active gesture set on the first registered
 * `zmk,input-processor-mouse-gesture` instance and rebuild its trie.
 *
 * The driver does NOT copy `patterns`, `pattern[]`, or `bindings[]` —
 * it stores pointers into them via the trie nodes. The caller MUST
 * keep all referenced storage alive until the next call (or program
 * end).
 *
 * Returns 0 on success, negative errno otherwise. When the driver is
 * compiled out (no `okay` instance in DT), returns -ENODEV.
 */
int zmk_mouse_gesture_runtime_set(const struct gesture_pattern *patterns,
                                   size_t count);

#ifdef __cplusplus
}
#endif

#endif /* ZMK_MOUSE_GESTURE_RUNTIME_H_ */
