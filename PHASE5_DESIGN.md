# Phase 5 Design: Runtime-configurable gesture set

This is a design note for a fork of `kot149/zmk-mouse-gesture` (v1) that
adds a small public C API so external modules can swap the gesture set
**at runtime**. The companion module
[`shakushakupanda/zmk-module-mouse-gesture-rpc`](https://github.com/shakushakupanda/zmk-module-mouse-gesture-rpc)
implements Phases 1–4 (RPC subsystem + NVS-persistent gesture store) but
its mutations don't yet affect the live trie. This patch closes that gap.

## Goals

- Expose `zmk_mouse_gesture_runtime_set(patterns, count)` that:
  1. Locks the input-processor instance,
  2. Saves the new pattern array reference,
  3. Rebuilds the gesture trie,
  4. Resets current matching state,
  5. Unlocks.
- Existing DTS gestures continue to work as defaults (used until a
  runtime override is set).
- Backwards-compatible: an in-tree user who doesn't link any RPC module
  sees the same compile-time behavior as today.

## Files changed (minimum surface)

### NEW: `include/zmk/mouse_gesture/runtime.h`

Moves `struct gesture_pattern` out of `.c` (was file-local) and declares
the runtime setter.

```c
#ifndef ZMK_MOUSE_GESTURE_RUNTIME_H_
#define ZMK_MOUSE_GESTURE_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>
#include <drivers/behavior.h>  /* for struct zmk_behavior_binding */

/* Same shape as the existing file-local struct in
 * src/input_processors/input_processor_mouse_gesture.c, just relocated
 * to a public header so external modules can construct one. */
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
 * `zmk,input-processor-mouse-gesture` instance.
 *
 * The driver keeps the supplied `patterns` pointer for the lifetime of
 * the next call — the caller MUST keep this array AND its embedded
 * pattern[]/bindings[] arrays alive until the next call (or program
 * end). The driver does not copy the data; it rebuilds the trie from
 * the supplied references.
 *
 * Returns 0 on success, negative errno on failure (-ENODEV if no
 * matching instance is registered, -EINVAL on bad input, etc.).
 */
int zmk_mouse_gesture_runtime_set(const struct gesture_pattern *patterns,
                                   size_t count);

#endif /* ZMK_MOUSE_GESTURE_RUNTIME_H_ */
```

### MODIFIED: `src/input_processors/input_processor_mouse_gesture.c`

Five small changes:

**1. Remove the file-local definition** of `struct gesture_pattern` (it now lives in
the header). Replace it with:

```c
#include <zmk/mouse_gesture/runtime.h>
```

**2. Extend `struct input_processor_mouse_gesture_data`** with two fields:

```c
struct input_processor_mouse_gesture_data {
    struct k_mutex lock;
    bool is_active;
    int32_t acc_x;
    int32_t acc_y;
    uint8_t last_direction;
    int64_t last_gesture_time;
    uint32_t event_count;
    int64_t last_reset_time;
    struct k_work_delayable idle_timeout_work;
    int64_t last_movement_time;
    struct gesture_node *current_node;
    const struct device *dev;
    struct gesture_node gesture_nodes_pool[MAX_GESTURE_TRIE_NODES];
    /* ...existing fields... */

    /* === Phase 5 additions === */
    const struct gesture_pattern *runtime_patterns;
    size_t                        runtime_pattern_count;
};
```

**3. Update `match_gesture_pattern_locked`** (and any other place that
reads `config->patterns` / `config->pattern_count`) to prefer the runtime
override when set:

```c
static const struct gesture_pattern *
mg_active_patterns(const struct device *dev) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    const struct input_processor_mouse_gesture_data   *data   = dev->data;
    return data->runtime_patterns ? data->runtime_patterns : config->patterns;
}

static size_t
mg_active_pattern_count(const struct device *dev) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    const struct input_processor_mouse_gesture_data   *data   = dev->data;
    return data->runtime_patterns ? data->runtime_pattern_count : config->pattern_count;
}
```

Then replace `config->patterns` / `config->pattern_count` with these
accessors at every call site (the existing `match_gesture_pattern_locked`
iterates patterns to fire bindings, but actually since the *trie node*
already holds a `pattern` pointer, only a couple of sites need updating).

**4. Add `zmk_mouse_gesture_runtime_set` definition** near the bottom of the file:

```c
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_input_processor_mouse_gesture)

int zmk_mouse_gesture_runtime_set(const struct gesture_pattern *patterns,
                                   size_t count) {
    /* Use instance 0 — we only support one mouse-gesture processor per
     * keyboard for now. */
    const struct device *dev = DEVICE_DT_GET(DT_INST(0, zmk_input_processor_mouse_gesture));
    if (!dev || !device_is_ready(dev)) {
        return -ENODEV;
    }
    struct input_processor_mouse_gesture_data *data = dev->data;
    if (!data) return -ENODEV;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->runtime_patterns      = patterns;
    data->runtime_pattern_count = count;
    /* Rebuild the trie from the new pattern set. */
    build_gesture_trie(data, patterns, count);
    /* Reset matching state. */
    data->current_node = data->gesture_trie_root; /* if such a field exists */
    clear_gesture_data_locked(data);
    k_mutex_unlock(&data->lock);
    return 0;
}

#else

int zmk_mouse_gesture_runtime_set(const struct gesture_pattern *patterns,
                                   size_t count) {
    (void)patterns; (void)count;
    return -ENODEV;
}

#endif
```

**5. (Optional) Hook into init** so build_gesture_trie is also seeded
from `data->runtime_patterns` if present at boot. Without this, after a
reboot the trie initially uses `config->patterns` (DTS defaults) until
something calls `zmk_mouse_gesture_runtime_set`. That's actually the
desired behavior for our RPC module — we want to load from NVS and
apply.

### MODIFIED (consumer side): `shakushakupanda/zmk-module-mouse-gesture-rpc:src/storage/gesture_store.c`

After every mutation (`mg_store_add`, `mg_store_update`, `mg_store_delete`,
`mg_store_reset_to_defaults`, and on `mg_store_init` after NVS load), call:

```c
#include <zmk/mouse_gesture/runtime.h>

/* These storage arrays live for the program lifetime; the kot149 driver
 * keeps references into them. */
static struct gesture_pattern    g_kot_patterns[MG_MAX_GESTURES];
static uint8_t                   g_kot_pattern_bytes[MG_MAX_GESTURES][MG_PATTERN_MAX];
static struct zmk_behavior_binding g_kot_bindings[MG_MAX_GESTURES][1]; /* one binding per gesture */

/* Map proto Direction (UP=0, RIGHT=1, DOWN=2, LEFT=3) to kot149
 * GESTURE_* bitmask (UP=1, DOWN=2, LEFT=4, RIGHT=8). */
static uint8_t proto_to_kot_direction(uint8_t d) {
    switch (d) {
    case 0: return 1; /* UP */
    case 1: return 8; /* RIGHT */
    case 2: return 2; /* DOWN */
    case 3: return 4; /* LEFT */
    default: return 1;
    }
}

static int sync_to_kot149(void) {
    size_t n = 0;
    for (size_t i = 0; i < MG_MAX_GESTURES && n < MG_MAX_GESTURES; i++) {
        if (!g_store[i].in_use || !g_store[i].enabled) continue;

        /* pattern */
        size_t plen = g_store[i].pattern_len;
        if (plen > MG_PATTERN_MAX) plen = MG_PATTERN_MAX;
        for (size_t j = 0; j < plen; j++) {
            g_kot_pattern_bytes[n][j] = proto_to_kot_direction(g_store[i].pattern[j]);
        }

        /* binding (we store one per gesture; kot149 supports many) */
        g_kot_bindings[n][0].behavior_dev = g_store[i].binding_behavior;
        g_kot_bindings[n][0].param1       = g_store[i].binding_param1;
        g_kot_bindings[n][0].param2       = g_store[i].binding_param2;

        g_kot_patterns[n] = (struct gesture_pattern){
            .bindings_len = 1,
            .bindings     = g_kot_bindings[n],
            .pattern_len  = plen,
            .wait_ms      = 0,
            .tap_ms       = 0,
            .pattern      = g_kot_pattern_bytes[n],
        };
        n++;
    }
    return zmk_mouse_gesture_runtime_set(g_kot_patterns, n);
}
```

Call `sync_to_kot149()` at the end of `mg_store_init`, `mg_store_add`,
`mg_store_update`, `mg_store_delete`, and `mg_store_reset_to_defaults`.

### MODIFIED: `zmk-config-moNa2-v2:config/west.yml`

Repoint the `zmk-mouse-gesture` project at the fork:

```yaml
projects:
  ...
  - name: zmk-mouse-gesture
    remote: shakushakupanda        # was: kot149
    revision: runtime-api          # new branch in the fork
```

(Add the `shakushakupanda` remote if it's not already there — moNa2's
dya-studio branch already has it, so this is just a remote rename.)

## Caveats & follow-ups

- **One binding per gesture** in our store today, vs. kot149 supports
  multiple. Not a hard limit, but the RPC proto only has a single
  `Binding` field. Phase 6 could extend the proto to a repeated
  `Binding[]`.
- **`gesture_pattern` struct moves to a public header** — that's an
  upstream-breaking change for anyone who relied on it being file-local.
  Should be safe: the existing kot149 codebase doesn't expose this
  publicly.
- **`build_gesture_trie` is currently `static`** — we don't need to
  change its visibility; the new public function lives in the same
  translation unit.
- **Memory ownership**: the kot149 driver stores raw pointers into our
  static arrays. Phase 5 must guarantee the arrays outlive the trie. A
  `static` lifetime in our store module satisfies this trivially.
- **Concurrent mutations**: kot149's lock is held during runtime_set, so
  we're safe vs. the matcher running in parallel.

## How to apply

1. Push `include/zmk/mouse_gesture/runtime.h` to fork (new file).
2. In the fork's `input_processor_mouse_gesture.c`, apply the five edits
   above. Push to a branch named `runtime-api` (don't break `v1`).
3. In our RPC module, add the `sync_to_kot149()` function and call it
   from store mutations.
4. In moNa2, change `west.yml` to point `zmk-mouse-gesture` at the fork
   with `revision: runtime-api`.
5. Build → flash → verify with DYA Studio: adding a gesture should
   immediately fire (after the next stroke that matches the new pattern).

## Testing checklist

- [ ] Boot with NVS empty: trie matches DTS defaults (Phase 2 behavior).
- [ ] Boot after NVS has data: trie matches stored gestures (runtime override).
- [ ] AddGesture via RPC → new pattern fires.
- [ ] UpdateGesture binding → new binding fires.
- [ ] DeleteGesture → old pattern no longer fires.
- [ ] ResetToDefaults → falls back to DTS defaults.
- [ ] Concurrency: gesture matching while DYA Studio is editing.
