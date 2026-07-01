# Master Spec — VideoScroll Multi-Instance Chain Probe + Blended Mixer

## Architecture Summary

The global "Video Scroll" waterfall becomes an insertable, multi-instance **chain probe module** (`ModuleType::VideoScroll`). Each instance is a pure pass-through tap: inside the RT image chain it captures the RGB scanline *as it flows at its position* into a per-instance lock-free SPSC ring (`processing/video_scroll.c`), then forwards the pixels downstream unchanged. Each instance owns a persisted **slot** (0..7) that keys both its preallocated RT ring (`video_scroll_instance(slot)`) and its automatable APVTS bank (`videoScroll{slot}_*`). A new `VideoMixerComponent` is the single message-thread consumer per ring: it owns one headless render-core per active slot, blends them into ONE master image (per-output level + Mix/Add/Screen), and is detachable/fullscreen. On load, a one-shot migration folds the legacy global waterfall into the first instance (slot 0), and `makeDefault()` seeds one instance immediately **before** the synth so it captures by construction. Everything user-facing — palette VIDEO section, drag-in, click-to-select-the-right-instance, contextual panel, mixer faders — is wired in PR2/PR3 across `ModuleCatalog`, `ChainModel`, `ChainRackComponent`, and `PluginEditor`.

## Key Risks & How the Spec Handles Them

- **W1 — Sampler short-circuit (Path A `has_sampler && mod_R` skips `image_chain_run`):** when the short-circuit fires, the executor still feeds every VideoScroll probe in that chain with the modulated/sampler frame, so the waterfall stays alive. Documented positional limitation: all probes in a sampler chain show the same post-sampler frame.
- **W2 — LuxWave-only chain (`synth[2]` inserts never executed):** documented v1 limitation — a probe captures only if its chain also contains LuxStral or LuxSynth. The plan still emits the insert harmlessly.
- **W3 — Synth-less chain (no synth → `image_chain_run` never runs):** documented v1 limitation — a probe must sit upstream of a LuxStral/LuxSynth to capture. `makeDefault`/migration always place the probe before a synth, so the default/migrated case captures.
- **RT-safety:** the only synth-thread touch is `video_scroll_capture_line` (bounded memcpy + one release store, no alloc/lock). Pause is a single `VS_ATOMIC(int)` written from the message thread as a plain `volatile` store (mirroring `lux_pitch.h`'s `LP_ATOMIC` discipline — **not** C11 `atomic_store_explicit`, which won't compile in the C++ TU). All draining/history/compositing/painting is message-thread.
- **Slot lifecycle:** slots are persisted per instance; `validateAndRepair` is **heal-only** (preserves valid unique slots, reassigns only `-1`/out-of-range/duplicate), so automation lanes never silently remap. Pool full (8 used) ⇒ `canInsert` returns false. The duplicate-type-per-chain rule is relaxed for VideoScroll only.
- **Single-reader-per-ring:** the **mixer is the sole consumer** of each slot ring. The contextual Zone-3 preview does **not** own its own core — it reads the mixer's per-slot rendered image via `VideoMixerComponent::imageForSlot(slot)`. Headless cores stop their own timer (`setHeadless(true)`) so only the mixer's timer drives them.
- **Generation single-writer:** the ring `generation` atomic is bumped only by the **producer** (synth thread, on discontinuity). UI Stop uses a separate UI-owned clear path (the consumer jumps its cursor and wipes its own image); it does **not** write the producer's `generation`.
- **Insert-count budget:** `CHAIN_PLAN_MAX_INSERTS = 10` (Pitch + Mask + up to 8 probes in one chain, per D2's relaxation). A `static_assert(CHAIN_PLAN_MAX_INSERTS >= 10)` fails the build loud if reduced.
- **Memory budget:** 8 instances × 256 slots × (8192×3 + 4) ≈ **50 MB** static BSS, zero-initialized once in the constructor before the synth thread starts. The only tuning knob is `VIDEO_SCROLL_RING_SLOTS`. The ring is newest-wins: at device line rate L and 60 Hz drain it holds `RING_SLOTS/L` seconds; on overrun the oldest in-frame lines are dropped (acceptable for a waterfall).
- **Compile blockers resolved:** `ChainModel::chains` is a public **member** (use `.chains`, not `.chains()`); `ChainModel::insert` is **3-arg** `(chainIdx, type, dropIdx)`; `VideoWindow::ContentComponent::display` converts from a value member to `unique_ptr`; the `video_scroll.h` include goes **inside** PluginProcessor.cpp's `extern "C"` block; `getRawParameterValue` results are null-guarded.

---

# PR1 — Plumbing + Parity (RT engine, no behavior change yet)

**Goal:** add the type/slot data model, the RT capture unit, and the image-chain/plan/executor wiring, with `makeDefault` seeding one probe **before** the synth. After PR1 the engine captures into rings but nothing renders from them yet (the global renderer still runs).

## Files touched
- NEW `vst/source/processing/video_scroll.h`
- NEW `vst/source/processing/video_scroll.c`
- `vst/source/processing/image_chain.h`
- `vst/source/processing/image_chain.c`
- `vst/source/processing/chain_plan.h`
- `vst/source/ui/ModuleCatalog.h`
- `vst/source/ui/ChainModel.h`
- `vst/source/ui/ChainModel.cpp`
- `vst/source/threading/multithreading.c`
- `vst/source/PluginProcessor.cpp` (init + `deriveAndPublishChainPlan` only)
- `vst/CMakeLists.txt`

---

## NEW FILE — `vst/source/processing/video_scroll.h`

```c
/*
 * video_scroll.h — pass-through RT image PROBE for the chain pipeline.
 *
 * A VideoScroll insert captures the RGB scanline AS IT FLOWS at its position in
 * a synth chain (IMAGE_CHAIN_INSERT_VIDEOSCROLL) into a per-instance preallocated
 * SPSC ring, then forwards the image downstream UNCHANGED. Display-only params
 * (speed/zoom/fade/invert/colour) live in APVTS and are applied UI-side.
 *
 * Threading:
 *   PRODUCER (single) : synthesis thread, video_scroll_capture_line().
 *                       RT-safe: bounded memcpy + one release store. No alloc/lock.
 *                       Owns write_index (release) AND generation (bumped on
 *                       discontinuity). On overrun it simply advances (newest wins).
 *   CONSUMER (single) : message/timer thread, video_scroll_ring_*().
 *                       Lock-free acquire loads. The drain cursor is owned by the
 *                       consumer component (not stored in this struct).
 *   CONTROL           : paused is written by the message thread (plain volatile
 *                       store) and read by the synth thread.
 *
 * Memory: per slot = 8192*3 + 4 ≈ 24,580 B; per instance (256 slots) ≈ 6.29 MB;
 *         pool (CHAIN_MAX_CHAINS=8) ≈ 50.3 MB BSS. Tune VIDEO_SCROLL_RING_SLOTS.
 */
#ifndef VIDEO_SCROLL_H
#define VIDEO_SCROLL_H

#include <stdint.h>
#include "chain_plan.h"   /* CHAIN_MAX_CHAINS — per-instance pool size */

/* Atomic discipline mirrors lux_pitch.h's LP_ATOMIC: on the C++ side fields are
 * plain `volatile T` (atomic for int/uint32 on all targets, written as plain
 * stores) and <atomic> is pulled in only for std::memory_order names; on the C
 * side they are real _Atomic T with <stdatomic.h>. This lets PluginProcessor.cpp
 * (C++) write `paused` with a simple volatile store while video_scroll.c (C11)
 * uses atomic_*_explicit. */
#ifdef __cplusplus
  #include <atomic>
  #define VS_ATOMIC(T) volatile T
extern "C" {
#else
  #include <stdatomic.h>
  #define VS_ATOMIC(T) _Atomic T
#endif

#define VIDEO_SCROLL_MAX_PIXELS  8192
#define VIDEO_SCROLL_RING_SLOTS  256                 /* power of two */
#define VIDEO_SCROLL_RING_MASK   (VIDEO_SCROLL_RING_SLOTS - 1)

/* One captured scanline (r/g/b only; UI derives luma for B&W). */
typedef struct {
    uint8_t r[VIDEO_SCROLL_MAX_PIXELS];
    uint8_t g[VIDEO_SCROLL_MAX_PIXELS];
    uint8_t b[VIDEO_SCROLL_MAX_PIXELS];
    int     pixel_count;
} VideoScrollSlot;

/* One capture instance (one pool slot). */
typedef struct VideoScrollState {
    VideoScrollSlot     slots[VIDEO_SCROLL_RING_SLOTS];
    VS_ATOMIC(uint32_t) write_index;   /* producer→consumer total pushes (release) */
    VS_ATOMIC(uint32_t) generation;    /* producer-only: bumped on discontinuity   */
    VS_ATOMIC(int)      paused;        /* message-thread write, synth-thread read   */
} VideoScrollState;

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */
void video_scroll_init(VideoScrollState *state);

/* ── Producer (synthesis thread — RT-safe) ─────────────────────────────────── */
/* Push one scanline (no-op when paused, NULL, or pixel_count<=0). */
void video_scroll_capture_line(VideoScrollState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int pixel_count);

/* ── Consumer (UI/timer thread — lock-free, single consumer) ───────────────── */
/* New lines available since last_read; clamped to RING_SLOTS (newest wins).
 * Writes the newest line's pixel count into *out_pixel_count (may be NULL). */
uint32_t video_scroll_ring_available(const VideoScrollState *state,
                                     uint32_t last_read, int *out_pixel_count);

/* Current total-pushes counter (acquire). Use as new read cursor after draining. */
uint32_t video_scroll_ring_writepos(const VideoScrollState *state);

/* Copy the slot at ABSOLUTE index `idx` (a total-pushes value) into caller
 * buffers (each >= VIDEO_SCROLL_MAX_PIXELS). Returns 1 on success, 0 if `idx`
 * is outside the live window. ON A 0 RETURN out_* ARE INDETERMINATE AND MUST NOT
 * BE RENDERED; *out_px is set to 0. Re-validates AFTER the copy to reject tears. */
int video_scroll_ring_get(const VideoScrollState *state, uint32_t idx,
                          uint8_t *out_r, uint8_t *out_g, uint8_t *out_b,
                          int *out_px);

/* ── Clear / generation ────────────────────────────────────────────────────── */
/* generation is PRODUCER-OWNED. The consumer reads it to blank history; it never
 * writes it (UI Stop wipes its own image by re-anchoring its cursor). */
uint32_t video_scroll_generation(const VideoScrollState *state);

/* ── Per-instance pool (mirrors lux_pitch) ─────────────────────────────────── */
extern VideoScrollState g_video_scroll_proc;
VideoScrollState *video_scroll_instance(int idx);  /* idx clamped to [0, CHAIN_MAX_CHAINS) */
void              video_scroll_init_all(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_SCROLL_H */
```

> **Design note (clear path):** there is deliberately **no** `video_scroll_request_clear`. UI Stop is handled entirely consumer-side (the component jumps its cursor to `writepos` and clears its own `juce::Image`), preserving the single-writer rule for `generation`. The producer bumps `generation` only on a true discontinuity (e.g. pixel-count change); the consumer compares it to its stored `lastGen_` to blank history.

---

## NEW FILE — `vst/source/processing/video_scroll.c`

```c
/*
 * video_scroll.c — SPSC ring + per-instance pool for the VideoScroll probe.
 * RT-safety: pure C, allocation-free, lock-free. See video_scroll.h.
 */
#include "video_scroll.h"
#include <string.h>

VideoScrollState g_video_scroll_proc;
static VideoScrollState s_video_scroll_extra[CHAIN_MAX_CHAINS - 1];

VideoScrollState *video_scroll_instance(int idx)
{
    if (idx <= 0) return &g_video_scroll_proc;
    if (idx >= CHAIN_MAX_CHAINS) idx = CHAIN_MAX_CHAINS - 1;
    return &s_video_scroll_extra[idx - 1];
}

void video_scroll_init_all(void)
{
    for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        video_scroll_init(video_scroll_instance(i));
}

void video_scroll_init(VideoScrollState *state)
{
    if (!state) return;
    memset(state->slots, 0, sizeof(state->slots));   /* ~6.3 MB, INIT ONLY (never RT) */
    atomic_init(&state->write_index, 0u);
    atomic_init(&state->generation,  0u);
    atomic_init(&state->paused,      0);
}

/* ── Producer (synthesis thread) ───────────────────────────────────────────── */
void video_scroll_capture_line(VideoScrollState *state,
                               const uint8_t *in_r, const uint8_t *in_g,
                               const uint8_t *in_b, int pixel_count)
{
    uint32_t w, slot;
    int px;

    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
        return;
    if (atomic_load_explicit(&state->paused, memory_order_relaxed))
        return;   /* paused → freeze; don't consume ring bandwidth */

    px = pixel_count;
    if (px > VIDEO_SCROLL_MAX_PIXELS) px = VIDEO_SCROLL_MAX_PIXELS;

    w    = atomic_load_explicit(&state->write_index, memory_order_relaxed);
    slot = w & VIDEO_SCROLL_RING_MASK;

    memcpy(state->slots[slot].r, in_r, (size_t)px);
    memcpy(state->slots[slot].g, in_g, (size_t)px);
    memcpy(state->slots[slot].b, in_b, (size_t)px);
    state->slots[slot].pixel_count = px;

    /* Release: slot contents visible before the incremented write_index. */
    atomic_store_explicit(&state->write_index, w + 1u, memory_order_release);
}

/* ── Consumer (UI/timer thread) ────────────────────────────────────────────── */
uint32_t video_scroll_ring_writepos(const VideoScrollState *state)
{
    if (!state) return 0u;
    return atomic_load_explicit(&state->write_index, memory_order_acquire);
}

uint32_t video_scroll_ring_available(const VideoScrollState *state,
                                     uint32_t last_read, int *out_pixel_count)
{
    uint32_t w, avail;
    if (out_pixel_count) *out_pixel_count = 0;
    if (!state) return 0u;

    w = atomic_load_explicit(&state->write_index, memory_order_acquire);
    avail = w - last_read;                    /* wrap-safe unsigned subtraction */
    if (avail > VIDEO_SCROLL_RING_SLOTS)
        avail = VIDEO_SCROLL_RING_SLOTS;      /* overrun clamp: newest window only */

    if (out_pixel_count && w != last_read)
        *out_pixel_count = state->slots[(w - 1u) & VIDEO_SCROLL_RING_MASK].pixel_count;
    return avail;
}

static int video_scroll_idx_live(uint32_t w, uint32_t idx)
{
    uint32_t age;
    if ((int32_t)(idx - w) >= 0) return 0;    /* not yet produced */
    age = w - idx;
    if (age > VIDEO_SCROLL_RING_SLOTS) return 0;  /* already overwritten */
    return 1;
}

int video_scroll_ring_get(const VideoScrollState *state, uint32_t idx,
                          uint8_t *out_r, uint8_t *out_g, uint8_t *out_b,
                          int *out_px)
{
    uint32_t w;
    const VideoScrollSlot *s;
    int px;

    if (out_px) *out_px = 0;
    if (!state || !out_r || !out_g || !out_b) return 0;

    w = atomic_load_explicit(&state->write_index, memory_order_acquire);
    if (!video_scroll_idx_live(w, idx)) return 0;

    s  = &state->slots[idx & VIDEO_SCROLL_RING_MASK];
    px = s->pixel_count;
    if (px < 0) px = 0;
    if (px > VIDEO_SCROLL_MAX_PIXELS) px = VIDEO_SCROLL_MAX_PIXELS;

    memcpy(out_r, s->r, (size_t)px);
    memcpy(out_g, s->g, (size_t)px);
    memcpy(out_b, s->b, (size_t)px);

    /* Re-validate after the copy: reject a tear if the producer lapped mid-copy.
     * On tear out_* are INDETERMINATE and must be discarded by the caller. */
    if (!video_scroll_idx_live(video_scroll_ring_writepos(state), idx))
        return 0;

    if (out_px) *out_px = px;
    return 1;
}

uint32_t video_scroll_generation(const VideoScrollState *state)
{
    if (!state) return 0u;
    return atomic_load_explicit(&state->generation, memory_order_acquire);
}
```

---

## `vst/source/processing/image_chain.h`

**BEFORE** (lines 35–38):
```c
/* Insert identifiers — also the tap indices in AudioImageBuffers. */
#define IMAGE_CHAIN_INSERT_LUXPITCH 0
#define IMAGE_CHAIN_INSERT_LUXMASK  1
#define IMAGE_CHAIN_NUM_INSERTS     2
```
**AFTER:**
```c
/* Insert identifiers.
 * IDs 0..1 (LUXPITCH/LUXMASK) double as AudioImageBuffers tap indices
 * (AUDIO_IMAGE_NUM_INSERT_TAPS == 2). VIDEOSCROLL (2) is a PASS-THROUGH PROBE
 * with NO tap slot: it captures into its own VideoScrollState ring and forwards
 * the frame unchanged. It is ONLY consumed by image_chain_run(), never by the
 * tap path (image_chain_process_inserts). */
#define IMAGE_CHAIN_INSERT_LUXPITCH    0
#define IMAGE_CHAIN_INSERT_LUXMASK     1
#define IMAGE_CHAIN_INSERT_VIDEOSCROLL 2
#define IMAGE_CHAIN_NUM_INSERTS        3
```

---

## `vst/source/processing/image_chain.c`

**Include (BEFORE, lines 10–15):**
```c
#include "image_chain.h"
#include "lux_pitch.h"
#include "lux_mask.h"
#include "../audio/buffers/audio_image_buffers.h"

#include <stdatomic.h>
```
**AFTER:**
```c
#include "image_chain.h"
#include "lux_pitch.h"
#include "lux_mask.h"
#include "video_scroll.h"
#include "../audio/buffers/audio_image_buffers.h"

#include <stdatomic.h>
```

**`s_tap_demand` initializer — normalize to `{0}`** so the 3-element array is unambiguously zeroed. **BEFORE** (line ~20):
```c
static int s_tap_demand[IMAGE_CHAIN_NUM_INSERTS] = {0, 0};
```
**AFTER:**
```c
static int s_tap_demand[IMAGE_CHAIN_NUM_INSERTS] = {0};  /* VIDEOSCROLL never demands a tap */
```

**`image_chain_run` switch — BEFORE** (lines 138–157):
```c
    for (int i = 0; i < num_inserts; i++)
    {
        const uint8_t *nr = cr, *ng = cg, *nb = cb;
        switch (insert_ids[i])
        {
            case IMAGE_CHAIN_INSERT_LUXPITCH:
                lux_pitch_process_frame((LuxPitchState *)insert_states[i],
                                        cr, cg, cb, pixel_count,
                                        luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXMASK:
                lux_mask_process_frame((LuxMaskState *)insert_states[i],
                                       cr, cg, cb, pixel_count,
                                       luxstral_num_octaves, &nr, &ng, &nb);
                break;
            default:
                break;   /* unknown insert → pass-through */
        }
        cr = nr; cg = ng; cb = nb;
    }
```
**AFTER:**
```c
    for (int i = 0; i < num_inserts; i++)
    {
        const uint8_t *nr = cr, *ng = cg, *nb = cb;
        switch (insert_ids[i])
        {
            case IMAGE_CHAIN_INSERT_LUXPITCH:
                lux_pitch_process_frame((LuxPitchState *)insert_states[i],
                                        cr, cg, cb, pixel_count,
                                        luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_LUXMASK:
                lux_mask_process_frame((LuxMaskState *)insert_states[i],
                                       cr, cg, cb, pixel_count,
                                       luxstral_num_octaves, &nr, &ng, &nb);
                break;
            case IMAGE_CHAIN_INSERT_VIDEOSCROLL:
                /* PASS-THROUGH PROBE: capture then forward unchanged. nr/ng/nb
                 * already equal cr/cg/cb. RT-safe SPSC push, no alloc/lock. */
                video_scroll_capture_line((VideoScrollState *)insert_states[i],
                                          cr, cg, cb, pixel_count);
                break;
            default:
                break;   /* unknown insert → pass-through */
        }
        cr = nr; cg = ng; cb = nb;
    }
```

---

## `vst/source/processing/chain_plan.h`

**BEFORE** (line 26):
```c
#define CHAIN_PLAN_MAX_INSERTS 4    /* max ordered processors before a synth */
```
**AFTER:**
```c
/* Max ordered inserts before a synth in ONE chain. Worst case allowed by the
 * model: Pitch + Mask + up to CHAIN_MAX_CHAINS (8) VideoScroll probes (D2
 * relaxes the per-chain duplicate rule for VideoScroll only) = 10. The
 * `num_inserts < CHAIN_PLAN_MAX_INSERTS` gate in deriveAndPublishChainPlan is a
 * defensive cap; at 10 it is unreachable for any legal model. states[] locals in
 * multithreading.c grow to 10*sizeof(void*) = 80 B — negligible stack. */
#define CHAIN_PLAN_MAX_INSERTS 10
```

**`insert_state_idx` doc — BEFORE** (lines 52–53):
```c
    int insert_id[CHAIN_PLAN_MAX_INSERTS];        /* IMAGE_CHAIN_INSERT_* in order */
    int insert_state_idx[CHAIN_PLAN_MAX_INSERTS]; /* pool slot per insert (chain id) */
```
**AFTER:**
```c
    int insert_id[CHAIN_PLAN_MAX_INSERTS];        /* IMAGE_CHAIN_INSERT_* in order */
    int insert_state_idx[CHAIN_PLAN_MAX_INSERTS]; /* pool slot per insert:
                                                   *  LUXPITCH/LUXMASK → chain-derived
                                                   *  state idx; VIDEOSCROLL → per-
                                                   *  instance slot (ModuleInstance.slot,
                                                   *  0..7). */
```

---

## `vst/source/ui/ModuleCatalog.h`

**Enum — BEFORE:**
```cpp
enum class ModuleType
{
    Sp3ctra = 0, Image, Video,     // SRC
    Pitch, Mask,                   // MIDI
    Sampler, Score,                // UTILS
    LuxStral, LuxSynth, LuxWave    // SYNTH
};
```
**AFTER:**
```cpp
enum class ModuleType
{
    Sp3ctra = 0, Image, Video,     // SRC
    Pitch, Mask,                   // MIDI
    Sampler, Score,                // UTILS
    LuxStral, LuxSynth, LuxWave,   // SYNTH
    VideoScroll                    // VIDEO (waterfall probe — pass-through, slotted)
};
```

**Category enum — BEFORE:** `enum class ModuleCat  { SRC, MIDI, UTILS, SYNTH };`
**AFTER:** `enum class ModuleCat  { SRC, MIDI, UTILS, SYNTH, VIDEO };`

**`moduleTable()` — change both `10`→`11` and append the row LAST:**
```cpp
inline const std::array<ModuleDesc, 11>& moduleTable()
{
    static const std::array<ModuleDesc, 11> table = {{
        // ... existing 10 rows UNCHANGED ...
        { ModuleType::LuxWave,     ModuleCat::SYNTH, ModuleRole::Synth,    "\xE2\x99\xAA LUXWAVE",  0xff8fd05a, "luxwaveEnabled", "LuxWave"  },
        { ModuleType::VideoScroll, ModuleCat::VIDEO, ModuleRole::Processor,"VIDEO SCROLL",          0xff5ad0c8, "",               "VideoScroll" },
    }};
    return table;
}
```

**`moduleCatLabel` — add the VIDEO case** before the closing brace of the switch:
```cpp
        case ModuleCat::VIDEO: return "VIDEO";
```

> `ModuleRole::Processor` (not Util — Util is singleton). CONTRACT-CONCERN: a Processor upstream of a synth flips `sourceChannelForSynth` to MODULATED, but VideoScroll's `image_chain_run` case is byte-identical pass-through, so the synth hears the same image. Implemented as contracted.

---

## `vst/source/ui/ChainModel.h`

**`ModuleInstance` — BEFORE:**
```cpp
struct ModuleInstance
{
    ModuleType  type;
    juce::Uuid  id;   ///< stable identity (survives reordering; Phase 2: per-instance params)
};
```
**AFTER:**
```cpp
struct ModuleInstance
{
    ModuleType  type;
    juce::Uuid  id;       ///< stable identity (survives reordering)
    int         slot{-1}; ///< per-instance APVTS bank / ring index, 0..7. Slotted
                          ///  types (VideoScroll) only; -1 otherwise.
};
```

**Add after `canInsert` declaration (public):**
```cpp
    //── VideoScroll per-instance slot pool ─────────────────────────────────────
    static constexpr int kMaxVideoSlots = 8;   // MUST equal CHAIN_MAX_CHAINS
    static bool isSlottedType(ModuleType t) noexcept { return t == ModuleType::VideoScroll; }
    int firstFreeVideoSlot(const juce::Uuid* movingId = nullptr) const;
    int videoSlotCount(const juce::Uuid* exclude = nullptr) const;
```

**Add the slot ValueTree id** beside the other `kTypeProp/kUuidProp/kVersionProp`:
```cpp
    static const juce::Identifier kSlotProp;    // "slot" (VideoScroll bank index)
```

---

## `vst/source/ui/ChainModel.cpp`

**Top — add include, static_asserts, and `kSlotProp` definition** after `#include "ChainModel.h"`:
```cpp
#include "../processing/chain_plan.h"

static_assert(ChainModel::kMaxVideoSlots == CHAIN_MAX_CHAINS,
              "ChainModel::kMaxVideoSlots must equal CHAIN_MAX_CHAINS (chain_plan.h)");
static_assert(CHAIN_PLAN_MAX_INSERTS >= 10,
              "VideoScroll probes share insert slots with Pitch/Mask; a single chain "
              "may hold Pitch+Mask + up to 8 probes (D2), so CHAIN_PLAN_MAX_INSERTS "
              "must be >= 10 or deriveAndPublishChainPlan silently drops probes.");
```
and with the other identifier definitions:
```cpp
const juce::Identifier ChainModel::kSlotProp { "slot" };
```

**Pool helpers** — insert after `chainHasType`, before `canInsert`:
```cpp
int ChainModel::videoSlotCount(const juce::Uuid* exclude) const
{
    int n = 0;
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isSlottedType(m.type)) continue;
            if (exclude != nullptr && m.id == *exclude) continue;
            ++n;
        }
    return n;
}

int ChainModel::firstFreeVideoSlot(const juce::Uuid* movingId) const
{
    bool used[kMaxVideoSlots] = { false };
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isSlottedType(m.type)) continue;
            if (movingId != nullptr && m.id == *movingId) continue;
            if (m.slot >= 0 && m.slot < kMaxVideoSlots) used[m.slot] = true;
        }
    for (int s = 0; s < kMaxVideoSlots; ++s)
        if (! used[s]) return s;
    return -1;
}
```

**`canInsert` — add pool gate + relax duplicate rule for slotted.** Insert the pool gate right after the `wantSource` line, and modify the per-chain duplicate check:
```cpp
    // (after: const bool wantSource = (role == ModuleRole::Source);)
    if (isSlottedType(type) && firstFreeVideoSlot(movingId) < 0)
        return false;   // pool full (8 used)
```
and in the per-chain loop, **BEFORE:**
```cpp
        if (m.type == type)
            return false;                         // no duplicate type per chain
```
**AFTER:**
```cpp
        if (m.type == type && ! isSlottedType(type))
            return false;   // duplicate type per chain (VideoScroll may repeat)
```

**`insert` — assign slot. BEFORE:**
```cpp
    auto& mods = chains[(size_t) chainIdx].modules;
    dropIdx = juce::jlimit(0, (int) mods.size(), dropIdx);
    mods.insert(mods.begin() + dropIdx, ModuleInstance{ type, juce::Uuid() });
    return true;
```
**AFTER:**
```cpp
    const int slot = isSlottedType(type) ? firstFreeVideoSlot() : -1;
    jassert(! isSlottedType(type) || slot >= 0);
    auto& mods = chains[(size_t) chainIdx].modules;
    dropIdx = juce::jlimit(0, (int) mods.size(), dropIdx);
    mods.insert(mods.begin() + dropIdx, ModuleInstance{ type, juce::Uuid(), slot });
    return true;
```

**`moveAcross` — pass `movingId` so the move keeps its own slot. BEFORE:**
```cpp
    const ModuleInstance moved = src[(size_t) from];
    if (! canInsert(toChain, moved.type))
        return false;
```
**AFTER:**
```cpp
    const ModuleInstance moved = src[(size_t) from];
    if (! canInsert(toChain, moved.type, &moved.id))   // moved keeps its slot
        return false;
```
(`moveWithin` already copies the full struct by value — slot rides along, no change.)

**`validateAndRepair` — heal-only slot pass (NOT unconditional renumber).** Add `int videoBudget = kMaxVideoSlots;` at the top, exempt slotted types from the per-chain duplicate drop and cap them by budget:
```cpp
        for (auto& m : ch.modules)
        {
            if (isSlottedType(m.type))   // exempt from per-chain duplicate rule
            {
                if (videoBudget <= 0) continue;   // 9th+ across model → drop
                --videoBudget;
                kept.push_back(m);
                continue;
            }
            // ... existing duplicate/source/singleton checks UNCHANGED ...
        }
```
Then replace any trailing slot-numbering with this **heal-only** second pass (after the `if (chains.empty())` guard):
```cpp
    // Second pass: HEAL only invalid/duplicate/-1 slots; PRESERVE valid ones
    // (slot stability ⇒ automation lanes never silently remap).
    bool used[kMaxVideoSlots] = { false };
    for (auto& ch : chains)
        for (auto& m : ch.modules)
        {
            if (! isSlottedType(m.type)) { m.slot = -1; continue; }
            if (m.slot >= 0 && m.slot < kMaxVideoSlots && ! used[m.slot])
                used[m.slot] = true;
            else
                m.slot = -1;   // invalid/collision → needs reassignment
        }
    for (auto& ch : chains)
        for (auto& m : ch.modules)
        {
            if (! isSlottedType(m.type) || m.slot >= 0) continue;
            for (int s = 0; s < kMaxVideoSlots; ++s)
                if (! used[s]) { m.slot = s; used[s] = true; break; }
            jassert(m.slot >= 0);   // guaranteed by videoBudget cap
        }
```

**`toValueTree` — write slot conditionally:**
```cpp
            if (isSlottedType(m.type) && m.slot >= 0)
                mt.setProperty(kSlotProp, m.slot, nullptr);
```

**`fromValueTree` — read slot (default -1):**
```cpp
            const int slot = isSlottedType(type)
                               ? (int) mt.getProperty(kSlotProp, juce::var(-1)) : -1;
            ch.modules.push_back(ModuleInstance{
                type, muuid.isNotEmpty() ? juce::Uuid(muuid) : juce::Uuid(), slot });
```

**`makeDefault` — seed ONE VideoScroll immediately BEFORE the synth (slot 0).** This is the canonical topology (resolves the S1/S6 conflict — probe is upstream of LuxStral so the executor captures it):
```cpp
    ChainModel m;
    // VideoScroll sits right BEFORE the synth (LuxStral) so deriveAndPublishChainPlan's
    // upstream-only fill() captures it. Placing it after the synth would never capture.
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::Pitch, ModuleType::Mask,
                              ModuleType::Sampler, ModuleType::Score,
                              ModuleType::VideoScroll, ModuleType::LuxStral }));
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::LuxSynth, ModuleType::LuxWave }));
    m.validateAndRepair();   // assigns slot 0 to the lone VideoScroll, normalises rest
    return m;
```

---

## `vst/source/threading/multithreading.c`

**Add include** after the existing `image_chain.h`/`lux_pitch.h` includes (match their relative-path style):
```c
#include "../processing/video_scroll.h"
```

**Path A states[] builder — replace the binary ternary** (lines ~737–748) with the 3-way switch (and `default: NULL`):
```c
            else if (spA->present && spA->num_inserts > 0)
            {
                void *states[CHAIN_PLAN_MAX_INSERTS];
                for (int i = 0; i < spA->num_inserts; i++)
                {
                    switch (spA->insert_id[i])
                    {
                        case IMAGE_CHAIN_INSERT_LUXPITCH:
                            states[i] = (void *)lux_pitch_instance(spA->insert_state_idx[i]); break;
                        case IMAGE_CHAIN_INSERT_LUXMASK:
                            states[i] = (void *)lux_mask_instance(spA->insert_state_idx[i]); break;
                        case IMAGE_CHAIN_INSERT_VIDEOSCROLL:
                            states[i] = (void *)video_scroll_instance(spA->insert_state_idx[i]); break;
                        default: states[i] = NULL; break;
                    }
                }
                image_chain_run(db->activeBuffer_R, db->activeBuffer_G, db->activeBuffer_B,
                                nb_pixels, g_sp3ctra_config.num_octaves,
                                spA->insert_id, states, spA->num_inserts,
                                &src_R, &src_G, &src_B);
            }
```

**Path A sampler short-circuit — feed probes (W1).** BEFORE (lines ~733–736):
```c
            if (spA->present && spA->has_sampler && mod_R)
            {
                src_R = mod_R; src_G = mod_G; src_B = mod_B;
            }
```
**AFTER:**
```c
            if (spA->present && spA->has_sampler && mod_R)
            {
                src_R = mod_R; src_G = mod_G; src_B = mod_B;
                /* W1: short-circuit skips image_chain_run; keep probes alive by
                 * feeding the modulated frame. All probes in a sampler chain
                 * therefore mirror the post-sampler frame (positional limitation). */
                for (int i = 0; i < spA->num_inserts; i++)
                    if (spA->insert_id[i] == IMAGE_CHAIN_INSERT_VIDEOSCROLL)
                        video_scroll_capture_line(
                            video_scroll_instance(spA->insert_state_idx[i]),
                            mod_R, mod_G, mod_B, nb_pixels);
            }
```

**Path B states[] builder** (lines ~781–792) — identical 3-way switch using `spB`.

> W2 (LuxWave-only) and W3 (synth-less chain) are documented limitations; no executor change. The plan still emits the inserts harmlessly.

---

## `vst/source/PluginProcessor.cpp` (PR1 portion)

**Include** — inside the existing `extern "C" { ... }` block (lines 6–20), after the lux_mask include line:
```cpp
    #include "processing/lux_mask.h"                          // LuxMask engine (g_lux_mask_proc)
    #include "processing/video_scroll.h"                      // VideoScroll capture-ring pool
```

**Init** — constructor (lines ~1182–1183), after `lux_mask_init_all();`:
```cpp
    lux_pitch_init_all();
    lux_mask_init_all();
    video_scroll_init_all();   // init 8 VideoScroll rings (RT pool), before synth thread starts
```

**`deriveAndPublishChainPlan` fill lambda** — emit VideoScroll inserts upstream of the synth. BEFORE (the upstream loop ~line 2433):
```cpp
        for (int i = 0; i < idx; ++i)
        {
            const ModuleType t = ch.modules[(size_t) i].type;
            if ((t == ModuleType::Pitch || t == ModuleType::Mask)
                && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                sp.insert_id[sp.num_inserts] = (t == ModuleType::Pitch)
                    ? IMAGE_CHAIN_INSERT_LUXPITCH : IMAGE_CHAIN_INSERT_LUXMASK;
                sp.insert_state_idx[sp.num_inserts] = stateIdx;
                sp.num_inserts++;
            }
            else if (t == ModuleType::Sampler) sp.has_sampler = 1;
            else if (t == ModuleType::Score)   sp.has_score   = 1;
        }
```
**AFTER:**
```cpp
        for (int i = 0; i < idx; ++i)
        {
            const ModuleInstance& mi = ch.modules[(size_t) i];
            const ModuleType t = mi.type;
            if ((t == ModuleType::Pitch || t == ModuleType::Mask)
                && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                sp.insert_id[sp.num_inserts] = (t == ModuleType::Pitch)
                    ? IMAGE_CHAIN_INSERT_LUXPITCH : IMAGE_CHAIN_INSERT_LUXMASK;
                sp.insert_state_idx[sp.num_inserts] = stateIdx;   // chain-derived Pitch/Mask slot
                sp.num_inserts++;
            }
            else if (t == ModuleType::VideoScroll
                     && mi.slot >= 0 && mi.slot < CHAIN_MAX_CHAINS
                     && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                // PASS-THROUGH PROBE: state idx is the PER-INSTANCE slot (0..7).
                sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_VIDEOSCROLL;
                sp.insert_state_idx[sp.num_inserts] = mi.slot;
                sp.num_inserts++;
            }
            else if (t == ModuleType::Sampler) sp.has_sampler = 1;
            else if (t == ModuleType::Score)   sp.has_score   = 1;
        }
```

---

## `vst/CMakeLists.txt` (PR1 portion)

After the `lux_pitch.c`/`lux_mask.c` lines (~228–229):
```cmake
        source/processing/lux_pitch.c
        source/processing/lux_mask.c
        source/processing/video_scroll.c
        source/processing/video_scroll.h
```

## PR1 verification
- Build clean (no `-Wswitch` on `ModuleCat`/`ModuleType`/the executor switches; both static_asserts pass).
- Load a fresh session → `chainModel` has one VideoScroll before LuxStral on Chain 1, slot 0.
- Add a temporary `log_debug` in `video_scroll_capture_line` (rate-limited) and confirm slot 0's `write_index` advances while audio runs and freezes when `paused` is set. Remove the log.
- Confirm existing audio/visual behavior is unchanged (probe is pass-through; global renderer still drives the visible waterfall).

---

# PR2 — Param Banks + Editor Selection/Contextual Panel

**Goal:** register the 8 banks + mixer params, add the slot→id helpers and RT pause sync, make the palette show VIDEO and the rack select the right instance, refactor `VideoScrollTab` to bind a slot, and route the contextual page. (Global renderer still shows the waterfall; mixer arrives in PR3.)

## Files touched
- `vst/source/PluginProcessor.h` (helpers + `activeVideoSlots` decl)
- `vst/source/PluginProcessor.cpp` (`createParameterLayout`, pause sync, `activeVideoSlots`)
- `vst/source/ui/ChainRackComponent.h` / `.cpp` (enum + 2 switches + slot callback)
- `vst/source/ui/ModuleCatalogComponent.cpp` (palette section list — locate & extend)
- `vst/source/video/VideoScrollTab.h` / `.cpp` (slot-parameterised)
- `vst/source/PluginEditor.h` / `.cpp` (contextual page + selection)

---

## `vst/source/PluginProcessor.h`

**Add free helpers** (header-inline, after includes):
```cpp
inline juce::String vsParam(int slot, const char* suffix)
{ return "videoScroll" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String vsMixParam(int slot, const char* suffix)
{ return "videoMix" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }
```
**Add accessor declaration** (public):
```cpp
    /** Slots (0..7) of every patched VideoScroll instance, lowest first.
     *  Message-thread snapshot of chainModel_. */
    std::vector<int> activeVideoSlots() const;
```

> Both UI builders consolidate on `vsParam`/`vsMixParam` — VideoScrollTab, VideoDisplayComponent, and VideoMixerComponent all call these (no parallel `pid()` member that can drift).

---

## `vst/source/PluginProcessor.cpp` — `createParameterLayout`

**Replace the entire global Video block (lines ~832–945)** with the banks, kept-global window geometry, and a reduced set of hidden legacy shims (migration-only). Drop the truly-dead globals (`videoScrollDirection/Exposure/BlendMode/Bpm/MidiSync`) by leaving them out — they were never read by the renderer.

```cpp
    // ── Video Scroll — per-instance banks (×8) + mixer (×8) ────────────────────
    {
        juce::StringArray videoModeNames { "0 deg", "90 deg", "180 deg", "270 deg" };
        juce::StringArray videoBlendNames{ "Mix", "Add", "Screen" };

        for (int N = 0; N < 8; ++N)
        {
            const juce::String p = "videoScroll" + juce::String(N) + "_";
            const juce::String mx = "videoMix"    + juce::String(N) + "_";
            const juce::String tag = "VS" + juce::String(N) + " ";

            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{p + "paused", 1}, tag + "Paused", true, kHiddenBool));  // transport latch
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{p + "mode", 1}, tag + "Mode", videoModeNames, 0));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "speed", 1}, tag + "Speed",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.33f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "zoom", 1}, tag + "Zoom",
                juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f), 1.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("x")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "linePos", 1}, tag + "Line Position",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "thickness", 1}, tag + "Line Thickness",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "fade", 1}, tag + "Fade",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{p + "compress", 1}, tag + "Compression",
                juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f));   // = legacy MaxDuration range
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{p + "invert", 1}, tag + "Invert Color", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{p + "colorMode", 1}, tag + "Color Mode (RGB)", false));

            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{mx + "level", 1}, "Mix " + tag + "Level",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{mx + "blend", 1}, "Mix " + tag + "Blend", videoBlendNames, 0));
        }
    }

    // ── Master window geometry (GLOBAL, kept) ──────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowWidth", 1}, "Video Window Width", 320, 2560, 800, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowHeight", 1}, "Video Window Height", 240, 1440, 600, kHiddenInt));

    // ── Legacy video params — HIDDEN, migration-only (one release). Read once by
    //    migrateLegacyVideoScroll(); never bound to UI/renderer. ────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollEnabled", 1}, "Video Scroll Enabled (legacy)", false, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollPaused", 1}, "Video Scroll Paused (legacy)", true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollMode", 1}, "Video Scroll Mode (legacy)",
        juce::StringArray{"0 deg","90 deg","180 deg","270 deg"}, 0, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollSpeed", 1}, "Video Scroll Speed (legacy)",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.33f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollZoom", 1}, "Video Scroll Zoom (legacy)",
        juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f), 1.0f, kHiddenFloat.withLabel("x")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLinePos", 1}, "Video Line Position (legacy)",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 1.0f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLineThickness", 1}, "Video Line Thickness (legacy)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollFade", 1}, "Video Fade (legacy)",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoInvertColor", 1}, "Video Invert Color (legacy)", false, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoColorMode", 1}, "Video Color Mode (legacy)", false, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollSource", 1}, "Video Scroll Source (legacy)",
        juce::StringArray{"LuxStral","LuxSynth/LuxWave","AllSynth"}, 0, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollMaxDuration", 1}, "Video Compression (legacy)",
        juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f, kHiddenFloat));
```

> Resolves the automatable-vs-hidden contradiction: all legacy ids are demoted to **hidden** (kept registered one release so migration can read them, but no host latches them). The previously-automatable `videoScrollSource`/`videoScrollBlendMode` etc. are not re-registered automatable — accepted automation-index churn for the dead globals, documented.

---

## `vst/source/PluginProcessor.cpp` — `activeVideoSlots` + RT pause sync

**`activeVideoSlots` (new method):**
```cpp
std::vector<int> Sp3ctraAudioProcessor::activeVideoSlots() const
{
    std::vector<int> out;
    for (const auto& chain : chainModel_.chains)        // member, NOT chains()
        for (const auto& m : chain.modules)
            if (m.type == ModuleType::VideoScroll && m.slot >= 0)
                out.push_back(m.slot);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
```

**Pause sync** — in `applyConfigurationToCore`, after the LuxMask config loop closes (~line 2849), before the `log_debug("VST", "Per-path routing: ...")`:
```cpp
        // ── Sync VideoScroll pause gate (S4). Banks indexed by SLOT (0..7).
        // The ONLY field the synth thread reads from the message thread. Written
        // as a plain volatile store (VS_ATOMIC == volatile int under C++), NOT
        // atomic_store_explicit (that is a C11 name, unavailable here). ──
        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
        {
            auto* pp = apvts.getRawParameterValue(vsParam(s, "paused"));
            jassert(pp != nullptr);
            const bool paused = pp && pp->load() > 0.5f;
            video_scroll_instance(s)->paused = paused ? 1 : 0;   // volatile store
        }
```

---

## `vst/source/ui/ChainRackComponent.h`

**Enum — add `VideoScroll` LAST** (keeps existing ordinals; `-Wswitch` now flags the 2 switches we fix below):
```cpp
enum class ChainBlockId
{
    Chain1Source = 0, Pitch, Mask, Sampler, Score, LuxStral,
    Chain2Source, LuxSynth, LuxWave,
    VideoScroll
};
```
**Add the slot callback** beside `onBlockSelected`:
```cpp
    /** Fired when a VideoScroll tile is clicked, carrying its instance slot so the
     *  editor can bind the contextual page to the right bank. */
    std::function<void(int slot)> onVideoBlockSelected;
```

---

## `vst/source/ui/ChainRackComponent.cpp`

**`chainBlockToModuleType` — add the case** before the `default`:
```cpp
        case ChainBlockId::VideoScroll: return ModuleType::VideoScroll;
```
**`instanceToBlockId` — add the case** before the `Sp3ctra/Image/Video` group:
```cpp
        case ModuleType::VideoScroll: return ChainBlockId::VideoScroll;
```
**`selectInstance` — fire the slot when a VideoScroll instance is clicked.** BEFORE:
```cpp
    if (notify && onBlockSelected)
    {
        int c = -1, i = -1;
        if (auto* m = model.find(id, c, i))
            onBlockSelected(instanceToBlockId(m->type, c));
    }
```
**AFTER:**
```cpp
    if (notify)
    {
        int c = -1, i = -1;
        if (auto* m = model.find(id, c, i))
        {
            // Per-instance highlighting already correct (tracked by Uuid above).
            if (m->type == ModuleType::VideoScroll && onVideoBlockSelected)
                onVideoBlockSelected(m->slot);
            if (onBlockSelected)
                onBlockSelected(instanceToBlockId(m->type, c));
        }
    }
```
> Tile highlighting is by `Uuid` (`blk->setSelected(blk->getUuid() == id)`), so only the clicked VideoScroll tile highlights even when several share `ChainBlockId::VideoScroll`. The flat block id is used only for contextual-page routing; the slot is carried separately.

---

## `vst/source/ui/ModuleCatalogComponent.cpp` (palette section list)

Locate the section-iteration list (grep for the existing `ModuleCat::SRC` / `ModuleCatLabel` usage in the catalogue component). It currently hardcodes `{ SRC, MIDI, UTILS, SYNTH }`. **Add `ModuleCat::VIDEO`:**

**BEFORE** (representative):
```cpp
    static const ModuleCat sections[] = {
        ModuleCat::SRC, ModuleCat::MIDI, ModuleCat::UTILS, ModuleCat::SYNTH
    };
```
**AFTER:**
```cpp
    static const ModuleCat sections[] = {
        ModuleCat::SRC, ModuleCat::MIDI, ModuleCat::UTILS, ModuleCat::SYNTH, ModuleCat::VIDEO
    };
```
> Without this, the VIDEO SCROLL palette tile never renders and the module is undraggable. If the component instead iterates `moduleTable()` directly grouping by `.category`, no change is needed — verify which pattern is in use during implementation; if it filters by an explicit list, extend that list.

---

## `vst/source/video/VideoScrollTab.h` / `.cpp`

**Header:** ctor `VideoScrollTab(Sp3ctraAudioProcessor&, int slot)`; add `const int slot_;`; **remove** `sourceCombo_`/`sourceAttach_` members and the `videoWindow_`/`openVideoWindow`/`closeVideoWindow`/`toggleDetachedWindow`/`requestFullscreenWindow`/`isVideoWindowOpen` members and methods; add `void setSlot(int);` and `static constexpr int kPreferredH = 430;`.

**`.cpp`:**
- Ctor: `: processor_(processor), slot_(juce::jlimit(0, 7, slot))`.
- **Delete** the entire SOURCE combo setup + attachment block (`sourceCombo_.addItem(...)` … `sourceAttach_ = ...`).
- Re-key every attachment via the shared helper:
  - `modeAttach_ → vsParam(slot_, "mode")`
  - `speedAttach_ → vsParam(slot_, "speed")`
  - `linePosAttach_ → vsParam(slot_, "linePos")`
  - `thicknessAttach_ → vsParam(slot_, "thickness")`
  - `zoomAttach_ → vsParam(slot_, "zoom")`
  - `fadeAttach_ → vsParam(slot_, "fade")`
  - `maxDurAttach_ → vsParam(slot_, "compress")`
- Keep the mode combo's existing "Scroll up/left/down/right" item labels (they map by **index** to the 4-choice param; do NOT rename them or you break the index mapping — the param's automation display showing "0 deg/90 deg/…" is a cosmetic divergence only).
- Collapse the SOURCE row in `computeLayout`/`paint`/`resized` (set `L.secSrc = L.ySource = -1000`; anchor SCROLL at `L.top + L.secExtra`; remove the `drawSection("SOURCE",…)`/`drawLabel("Source",…)` and `sourceCombo_.setBounds(...)`).
- Add `setSlot(int)`: rebuild all attachments against the new bank (reset each `unique_ptr` then re-`make_unique` with the new ids).
- **Delete** the `openVideoWindow`/`closeVideoWindow`/`toggleDetachedWindow`/`requestFullscreenWindow` bodies (window ownership moves to the mixer in PR3).

---

## `vst/source/PluginEditor.h` / `.cpp`

**Header:** add `#include "video/VideoScrollTab.h"`; add members:
```cpp
    std::unique_ptr<VideoScrollTab> videoScrollPage;
    int selectedVideoSlot_ { -1 };
```

**`.cpp` construction** (after the other Zone-3 pages):
```cpp
    videoScrollPage = std::make_unique<VideoScrollTab>(audioProcessor, /*slot*/ 0);
    zone3Content.addChildComponent(videoScrollPage.get());
```

**Wire the rack callback** (near the existing `chainRack->onModelChanged` at line 42):
```cpp
    chainRack->onVideoBlockSelected = [this](int slot)
    {
        selectedVideoSlot_ = slot;
        if (videoScrollPage) videoScrollPage->setSlot(slot);
        selectBlock(ChainBlockId::VideoScroll);
    };
```

**`selectBlock` switch — add the case** (exhaustive, no default — must be added to compile):
```cpp
        case ChainBlockId::VideoScroll:
            sources = { VisualizerMode::MODULATED };   // probe shows chain context
            break;
```

**`applyZone3Visibility`** — add:
```cpp
    if (videoScrollPage)
        videoScrollPage->setVisible(play && id == ChainBlockId::VideoScroll);
```

**`layoutZone3` PLAY-face switch** — add:
```cpp
        case ChainBlockId::VideoScroll:
            top = videoScrollPage.get();
            topMinH = VideoScrollTab::kPreferredH;
            break;
```

**`blockHasSetup`** — add `case ChainBlockId::VideoScroll: return false;` (no device/network setup). With `enableParamId("VideoScroll") == ""`, the power button auto-hides via the existing empty-id guard.

## PR2 verification
- Palette shows a **VIDEO** section with a draggable VIDEO SCROLL tile.
- Drag two VideoScroll into one chain → both accepted (relaxed duplicate rule); a 9th anywhere is rejected (pool full).
- Click each VideoScroll tile → only that tile highlights; the contextual VideoScrollTab binds to the correct `videoScroll{slot}_*` bank (verify a slider move writes the right param id).
- Toggle `videoScroll{slot}_paused` and confirm the corresponding ring's `write_index` freezes (temporary log), proving the pause sync and slot mapping.

---

# PR3 — Mixer (blend) + Per-Instance Renderer

**Goal:** refactor `VideoDisplayComponent` to a per-slot render-core fed by the ring; add `VideoMixerComponent` (sole ring consumer) that blends active instances into one master; rewire Zone-4 column + `VideoWindow` to host the master; make the Zone-3 preview read the mixer's per-slot image (single-reader-per-ring).

## Files touched
- `vst/source/video/VideoDisplayComponent.h` / `.cpp` (per-slot core, headless mode)
- NEW `vst/source/ui/VideoMixerComponent.h` / `.cpp`
- `vst/source/video/VideoWindow.h` / `.cpp` (host mixer OR single preview)
- `vst/source/ui/WaterfallColumnComponent.h` / `.cpp` (host the one mixer; re-parent on detach)
- `vst/source/PluginEditor.cpp` (`onModelChanged` → refresh mixer; preview reads mixer image)
- `vst/CMakeLists.txt` (add `VideoMixerComponent`)

---

## `VideoDisplayComponent` refactor (key edits)

- **Ctor** `VideoDisplayComponent(Sp3ctraAudioProcessor&, int slot)`; store `int slot_`; remove `CaptureThread`, `frameRing_`, `ringWriteIdx_/ReadIdx_`, all `AudioImageBuffers`/`videoScrollSource`/`config_loader` includes and the AllSynth path. Include only `processing/video_scroll.h` (inside an `extern "C"`).
- Add `uint32_t readIdx_{0}` (consumer cursor) and `uint32_t lastGen_{0}`.
- Add **headless mode** that stops the component's own timer so the mixer is the only driver:
```cpp
void VideoDisplayComponent::setHeadless(bool h)
{
    headless_ = h;
    if (h) stopTimer(); else startTimerHz(kTimerFps);
}
```
- **Param reads** go through null-guarded helpers using the shared `vsParam`:
```cpp
float VideoDisplayComponent::fParam(const char* s) const
{
    auto* p = processor_.getAPVTS().getRawParameterValue(vsParam(slot_, s));
    jassert(p != nullptr); return p ? p->load() : 0.0f;
}
bool VideoDisplayComponent::bParam(const char* s) const { return fParam(s) > 0.5f; }
int  VideoDisplayComponent::iParam(const char* s) const { return (int) fParam(s); }
```
- **`drainRing`** (message thread) replaces CaptureThread. Reads `video_scroll_ring_available(st, readIdx_, &pc)`, copies the newest `take` lines into a `kStageDepth` scratch ring via `video_scroll_ring_get` (skipping torn lines where it returns 0), derives gray, then `readIdx_ = video_scroll_ring_writepos(st)`.
- **`timerCallback`** (on-screen only): skip stepping when `headless_`; on `gen != lastGen_` clear history; `scrollStep()` then `repaint()`.
- **`scrollStep`** operates on `bufW_/bufH_/compH_` (drop the unused `targetW/targetH`); reads params via helpers; sources lines from the scratch stage ring; otherwise identical ping-pong/birth-line logic.
- **`buildLineImage`** sources from the stage ring, reads `invert`/`colorMode` per-instance.
- **`allocateScrollBuffer(int w, int h)`** takes explicit size; all images created with `juce::SoftwareImageType()` (pixelStride==3 guaranteed).
- **`renderInto(juce::Graphics&, int outW, int outH)`** holds the moved warp/compress/fade/zoom/rotate body (compress reads `fParam("compress")`).
- **Headless render for the mixer** — note the size is **fixed** by the mixer (`kRenderW×kRenderH`), so the core never thrashes `allocateScrollBuffer`:
```cpp
void VideoDisplayComponent::renderHeadless(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    allocateScrollBuffer(w, h);
    if (auto* st = video_scroll_instance(slot_))
    {
        const uint32_t gen = video_scroll_generation(st);
        if (gen != lastGen_) { lastGen_ = gen; clearHistory(); }
    }
    scrollStep();
    (void) getRenderedImage(w, h);
}

const juce::Image& VideoDisplayComponent::getRenderedImage(int w, int h)
{
    if (renderedImage_.getWidth() != w || renderedImage_.getHeight() != h)
        renderedImage_ = juce::Image(juce::Image::RGB, juce::jmax(1, w), juce::jmax(1, h),
                                     false, juce::SoftwareImageType());
    juce::Graphics g(renderedImage_);
    g.fillAll(juce::Colours::black);
    renderInto(g, w, h);
    return renderedImage_;
}
```
- **`setSlot(int)`** resets `readIdx_`/`lastGen_` and clears history.

> The global `getVideoScrollClearGen()`/`requestVideoScrollClear()`/`videoScrollClearGen` become dead. Their `setStateInformation` call-site (forcing legacy paused + clear) stays operating on the legacy id harmlessly; the global atomic + accessors are retired in PR4 cleanup.

---

## NEW FILE — `vst/source/ui/VideoMixerComponent.h`

```cpp
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../video/VideoDisplayComponent.h"
#include <memory>
#include <vector>

/** Blends every patched VideoScroll instance into ONE master waterfall.
 *  SOLE consumer of each slot ring: owns one HEADLESS VideoDisplayComponent per
 *  active slot (setHeadless(true) — driven only by this mixer's timer). Composites
 *  per-instance images via per-output level (videoMix{N}_level) and blend mode
 *  (videoMix{N}_blend). Detachable/fullscreen via VideoWindow.
 *  Message-thread only. */
class VideoMixerComponent : public juce::Component, private juce::Timer
{
public:
    explicit VideoMixerComponent(Sp3ctraAudioProcessor& proc);
    ~VideoMixerComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    std::function<void()> onFullscreenRequested;

    void refreshStrips();                 // re-derive cores/rows from activeVideoSlots()
    const juce::Image& getMasterImage() const noexcept { return master_; }
    /** Per-slot rendered image — Zone-3 preview reads this so no second ring drain. */
    const juce::Image* imageForSlot(int slot) const;

private:
    void timerCallback() override;
    void composite();

    struct Strip
    {
        int slot;
        std::unique_ptr<VideoDisplayComponent> core;   // HEADLESS
        juce::Label    nameLabel;
        juce::Slider   levelSlider;
        juce::ComboBox blendCombo;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>  levelAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> blendAttach;
    };

    Sp3ctraAudioProcessor& processor_;
    std::vector<std::unique_ptr<Strip>> strips_;
    juce::Image master_;

    static constexpr int kTimerFps = 60;
    static constexpr int kRowH = 26;
    static constexpr int kRenderW = 320, kRenderH = 240;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerComponent)
};
```

---

## NEW FILE — `vst/source/ui/VideoMixerComponent.cpp`

```cpp
#include "VideoMixerComponent.h"
#include <algorithm>

VideoMixerComponent::VideoMixerComponent(Sp3ctraAudioProcessor& proc) : processor_(proc)
{
    setOpaque(true);
    master_ = juce::Image(juce::Image::RGB, kRenderW, kRenderH, false, juce::SoftwareImageType());
    refreshStrips();
    startTimerHz(kTimerFps);
}
VideoMixerComponent::~VideoMixerComponent() { stopTimer(); }

void VideoMixerComponent::mouseDoubleClick(const juce::MouseEvent&)
{ if (onFullscreenRequested) onFullscreenRequested(); }

const juce::Image* VideoMixerComponent::imageForSlot(int slot) const
{
    for (auto& s : strips_)
        if (s->slot == slot) return &s->core->getRenderedImage(kRenderW, kRenderH);
    return nullptr;
}

void VideoMixerComponent::refreshStrips()
{
    const std::vector<int> slots = processor_.activeVideoSlots();

    for (int i = (int) strips_.size() - 1; i >= 0; --i)
        if (std::find(slots.begin(), slots.end(), strips_[i]->slot) == slots.end())
            strips_.erase(strips_.begin() + i);

    for (int slot : slots)
    {
        if (std::any_of(strips_.begin(), strips_.end(),
                        [slot](const std::unique_ptr<Strip>& s){ return s->slot == slot; }))
            continue;

        auto s = std::make_unique<Strip>();
        s->slot = slot;
        s->core = std::make_unique<VideoDisplayComponent>(processor_, slot);
        s->core->setHeadless(true);            // mixer is the only driver

        s->nameLabel.setText("VS " + juce::String(slot), juce::dontSendNotification);
        addAndMakeVisible(s->nameLabel);

        s->levelSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        s->levelSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
        addAndMakeVisible(s->levelSlider);
        s->levelAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor_.getAPVTS(), vsMixParam(slot, "level"), s->levelSlider);

        s->blendCombo.addItem("Mix", 1); s->blendCombo.addItem("Add", 2); s->blendCombo.addItem("Screen", 3);
        addAndMakeVisible(s->blendCombo);
        s->blendAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor_.getAPVTS(), vsMixParam(slot, "blend"), s->blendCombo);

        strips_.push_back(std::move(s));
    }

    std::sort(strips_.begin(), strips_.end(),
              [](const std::unique_ptr<Strip>& a, const std::unique_ptr<Strip>& b){ return a->slot < b->slot; });
    resized();
    repaint();
}

void VideoMixerComponent::timerCallback()
{
    for (auto& s : strips_) s->core->renderHeadless(kRenderW, kRenderH);
    composite();
    repaint();
}

// Blend model (resolves the order-dependent-Mix ambiguity):
//   Pass 1 (Mix-mode strips): weighted average  acc += src*L, sumW += L.
//   Master_mix = acc / max(1, sumW)   (a single Mix instance at any L shows undimmed).
//   Pass 2 (Add/Screen strips, in slot order): applied on top of Master_mix.
//     Add    : d = min(255, d + src*L)
//     Screen : d = 255 - (255-d)*(255 - src*L)/255
// All math on SoftwareImageType RGB (pixelStride==3 guaranteed).
void VideoMixerComponent::composite()
{
    if (master_.getWidth() != kRenderW || master_.getHeight() != kRenderH)
        master_ = juce::Image(juce::Image::RGB, kRenderW, kRenderH, false, juce::SoftwareImageType());

    std::vector<float> accR((size_t) kRenderW * kRenderH, 0.f),
                       accG(accR.size(), 0.f), accB(accR.size(), 0.f), sumW(accR.size(), 0.f);

    // Pass 1 — Mix strips → weighted average accumulators.
    for (auto& s : strips_)
    {
        const int blend = (int) processor_.getAPVTS().getRawParameterValue(vsMixParam(s->slot, "blend"))->load();
        if (blend != 0) continue;
        auto* lp = processor_.getAPVTS().getRawParameterValue(vsMixParam(s->slot, "level"));
        const float L = lp ? juce::jlimit(0.f, 1.f, lp->load()) : 0.f;
        if (L <= 0.f) continue;
        const juce::Image& srcImg = s->core->getRenderedImage(kRenderW, kRenderH);
        if (!srcImg.isValid()) continue;
        const juce::Image::BitmapData src(srcImg, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < kRenderH; ++y)
        {
            const uint8_t* sl = src.getLinePointer(y);
            for (int x = 0; x < kRenderW; ++x)
            {
                const auto* sp = reinterpret_cast<const juce::PixelRGB*>(sl + x * src.pixelStride);
                const size_t k = (size_t) y * kRenderW + x;
                accR[k] += sp->getRed() * L; accG[k] += sp->getGreen() * L; accB[k] += sp->getBlue() * L;
                sumW[k] += L;
            }
        }
    }

    juce::Image::BitmapData dst(master_, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < kRenderH; ++y)
    {
        uint8_t* dl = dst.getLinePointer(y);
        for (int x = 0; x < kRenderW; ++x)
        {
            const size_t k = (size_t) y * kRenderW + x;
            const float w = juce::jmax(1.f, sumW[k]);
            auto* dp = reinterpret_cast<juce::PixelRGB*>(dl + x * dst.pixelStride);
            dp->setARGB(255,
                (juce::uint8) juce::jlimit(0, 255, (int)(accR[k] / w)),
                (juce::uint8) juce::jlimit(0, 255, (int)(accG[k] / w)),
                (juce::uint8) juce::jlimit(0, 255, (int)(accB[k] / w)));
        }
    }

    // Pass 2 — Add/Screen strips on top, in slot order.
    for (auto& s : strips_)
    {
        const int blend = (int) processor_.getAPVTS().getRawParameterValue(vsMixParam(s->slot, "blend"))->load();
        if (blend == 0) continue;
        auto* lp = processor_.getAPVTS().getRawParameterValue(vsMixParam(s->slot, "level"));
        const float L = lp ? juce::jlimit(0.f, 1.f, lp->load()) : 0.f;
        if (L <= 0.f) continue;
        const juce::Image& srcImg = s->core->getRenderedImage(kRenderW, kRenderH);
        if (!srcImg.isValid()) continue;
        const juce::Image::BitmapData src(srcImg, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < kRenderH; ++y)
        {
            const uint8_t* sl = src.getLinePointer(y);
            uint8_t* dl = dst.getLinePointer(y);
            for (int x = 0; x < kRenderW; ++x)
            {
                const auto* sp = reinterpret_cast<const juce::PixelRGB*>(sl + x * src.pixelStride);
                auto* dp = reinterpret_cast<juce::PixelRGB*>(dl + x * dst.pixelStride);
                const int sr = (int)(sp->getRed() * L), sg = (int)(sp->getGreen() * L), sb = (int)(sp->getBlue() * L);
                int dr = dp->getRed(), dg = dp->getGreen(), db = dp->getBlue();
                if (blend == 1) { dr = juce::jmin(255, dr + sr); dg = juce::jmin(255, dg + sg); db = juce::jmin(255, db + sb); }
                else            { dr = 255 - ((255 - dr)*(255 - sr)/255); dg = 255 - ((255 - dg)*(255 - sg)/255); db = 255 - ((255 - db)*(255 - sb)/255); }
                dp->setARGB(255, (juce::uint8) juce::jlimit(0,255,dr), (juce::uint8) juce::jlimit(0,255,dg), (juce::uint8) juce::jlimit(0,255,db));
            }
        }
    }
}

void VideoMixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    const int stripH = (int) strips_.size() * kRowH;
    const int previewH = juce::jmax(0, getHeight() - stripH);
    if (master_.isValid() && previewH > 0)
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(master_, 0, 0, getWidth(), previewH, 0, 0, master_.getWidth(), master_.getHeight());
    }
    g.setColour(juce::Colour(0xff222230)); g.fillRect(0, previewH, getWidth(), stripH);
}

void VideoMixerComponent::resized()
{
    const int stripH = (int) strips_.size() * kRowH;
    int y = juce::jmax(0, getHeight() - stripH);
    const int nameW = 48, blendW = 80, pad = 4;
    for (auto& s : strips_)
    {
        int x = pad;
        s->nameLabel.setBounds(x, y + 2, nameW, kRowH - 4); x += nameW + pad;
        const int levelW = juce::jmax(60, getWidth() - x - blendW - 2 * pad);
        s->levelSlider.setBounds(x, y + 3, levelW, kRowH - 6); x += levelW + pad;
        s->blendCombo.setBounds(x, y + 3, blendW, kRowH - 6);
        y += kRowH;
    }
}
```

---

## `VideoWindow.h` / `.cpp` (host mixer OR single preview)

Convert the `ContentComponent::display` **value member** to two `unique_ptr`s and take a `VideoMixerComponent&` for the master case (no second mixer is ever constructed):

**`VideoWindow.h`:**
```cpp
class VideoWindow : public juce::DocumentWindow
{
public:
    /** Master: pass an existing mixer to re-parent (slot ignored). */
    VideoWindow(Sp3ctraAudioProcessor& proc, VideoMixerComponent& masterToHost);
    /** Single-instance preview for `slot`. */
    VideoWindow(Sp3ctraAudioProcessor& proc, int slot);
    VideoDisplayComponent* getDisplay() noexcept;
    void toggleFullscreen();
private:
    class ContentComponent : public juce::Component
    {
    public:
        ContentComponent(Sp3ctraAudioProcessor& proc, VideoWindow& owner, int slot,
                         VideoMixerComponent* hostedMixer);
        void resized() override;
        std::unique_ptr<VideoDisplayComponent> display;   // slot >= 0 (owns)
        VideoMixerComponent*                   mixer{nullptr}; // re-parented, NOT owned
    };
    std::unique_ptr<ContentComponent> content_;
};
```

**`VideoWindow.cpp` — ContentComponent ctor (remove `display(proc)` from the init list):**
```cpp
VideoWindow::ContentComponent::ContentComponent(Sp3ctraAudioProcessor& proc, VideoWindow& owner,
                                                int slot, VideoMixerComponent* hostedMixer)
{
    if (hostedMixer != nullptr)
    {
        mixer = hostedMixer;
        addAndMakeVisible(*mixer);                       // re-parent (owner keeps ownership)
        mixer->onFullscreenRequested = [&owner]{ owner.toggleFullscreen(); };
    }
    else
    {
        display = std::make_unique<VideoDisplayComponent>(proc, slot);
        addAndMakeVisible(*display);
        display->onFullscreenRequested = [&owner]{ owner.toggleFullscreen(); };
    }
}
void VideoWindow::ContentComponent::resized()
{
    auto* c = display ? (juce::Component*) display.get() : (juce::Component*) mixer;
    if (c) c->setBounds(getLocalBounds());
}
VideoDisplayComponent* VideoWindow::getDisplay() noexcept
{ return content_ ? content_->display.get() : nullptr; }
```
Window geometry persistence (`videoWindowWidth/Height`) stays as-is.

---

## `WaterfallColumnComponent.h` / `.cpp`

- Replace `std::unique_ptr<VideoScrollTab> videoTab;` with `std::unique_ptr<VideoMixerComponent> mixer_;` and `std::unique_ptr<VideoWindow> masterWindow_;`. Include `VideoMixerComponent.h` (drop `VideoScrollTab.h`).
- Add `void refreshMixer() { if (mixer_) mixer_->refreshStrips(); }` and `VideoMixerComponent* mixer() { return mixer_.get(); }`.
- Ctor: `mixer_ = std::make_unique<VideoMixerComponent>(processor); addAndMakeVisible(*mixer_);` (no viewport).
- `resized()`: `mixer_->setBounds(contentArea);`.
- Detach/fullscreen: **re-parent the single mixer** into/out of the window (never build a second):
```cpp
void WaterfallColumnComponent::openMasterWindow()
{
    if (masterWindow_) return;
    masterWindow_ = std::make_unique<VideoWindow>(processor, *mixer_);   // re-parents mixer_
}
void WaterfallColumnComponent::closeMasterWindow()
{
    masterWindow_.reset();
    addAndMakeVisible(*mixer_);   // re-parent back into the column
    resized();
}
```
- Stop button: per-instance — there is no producer-side clear; instead bump each on-screen core's clear by resetting the mixer cores' history. Simplest: `mixer_->refreshStrips()` is overkill; expose `mixer_->clearAll()` that calls `core->setSlot(core->getSlot())` (re-anchors cursor + clears history) for each strip. Remove the old global play/pause/clear transport (pause is per-instance now).
- Remove all `videoScrollPaused`/`videoScrollSource` attachments and the per-instance display toggles that lived here.

---

## `PluginEditor.cpp` — refresh hook + preview-reads-mixer

**`onModelChanged` lambda (the single authoritative body):**
```cpp
    chainRack->onModelChanged = [this]
    {
        layoutZones();
        if (waterfallColumn) waterfallColumn->refreshMixer();   // dynamic fader strip
    };
```

**Zone-3 preview reads the mixer image (single-reader-per-ring):** `VideoScrollTab` does **not** own a render core. If the tab shows a live preview, it asks the column's mixer: add to the tab a `std::function<const juce::Image*(int)> imageProvider` set by the editor to `[this](int slot){ return waterfallColumn && waterfallColumn->mixer() ? waterfallColumn->mixer()->imageForSlot(slot) : nullptr; }`, and the tab paints that image (no second drain of the slot ring).

---

## `vst/CMakeLists.txt` (PR3 portion)

Add the mixer; keep `VideoDisplayComponent`/`VideoWindow`/`VideoScrollTab`:
```cmake
        source/ui/VideoMixerComponent.cpp
        source/ui/VideoMixerComponent.h
```

## PR3 verification
- One probe patched → master shows its waterfall; level fader dims it; Add/Screen blend visibly differs from Mix.
- Two probes (one Mix, one Add) → master = averaged Mix base with the Add strip lit on top; reordering slots does not change the Mix base (order-independent), confirming the two-pass model.
- Detach master → the same mixer instance moves to the window (no stutter, no double-drain); close → returns to the column.
- Open the Zone-3 preview for a slot while the master is visible → no visible line-stealing (preview reads `imageForSlot`, not the ring).

---

# PR4 — Migration + Cleanup

**Goal:** fold the legacy global waterfall into slot-0 on load (idempotent, copies legacy values even on no-CHAINS sessions), remove the global system's dead code, and finalize CMake.

## Files touched
- `vst/source/PluginProcessor.h` (decl) / `.cpp` (`migrateLegacyVideoScroll`, call sites, dead-code removal)
- `vst/CMakeLists.txt` (remove `WaterfallColumnComponent` once deleted — see note)

---

## `PluginProcessor.h`
```cpp
    void loadChainModelFromState();
    /** One-shot legacy→instance migration (guard prop "videoScrollMigratedV1").
     *  Ensures a slot-0 VideoScroll exists upstream of the target synth and copies
     *  legacy videoScroll* into videoScroll0_*. Idempotent. Message-thread only. */
    void migrateLegacyVideoScroll();
```

## `PluginProcessor.cpp` — `migrateLegacyVideoScroll`

```cpp
void Sp3ctraAudioProcessor::migrateLegacyVideoScroll()
{
    auto& state = apvts.state;
    if ((int) state.getProperty("videoScrollMigratedV1", 0) != 0)
        return;

    // 1) Pick the target synth/chain from the legacy source selection.
    int srcSel = 0;
    if (auto* p = apvts.getRawParameterValue("videoScrollSource")) srcSel = (int) p->load();
    const ModuleType targetSynth = (srcSel == 1) ? ModuleType::LuxSynth : ModuleType::LuxStral;

    int targetChain = -1, synthIdx = -1;
    for (int c = 0; c < chainModel_.numChains() && targetChain < 0; ++c)
    {
        const auto& mods = chainModel_.chains[(size_t) c].modules;
        for (int i = 0; i < (int) mods.size(); ++i)
            if (mods[i].type == targetSynth) { targetChain = c; synthIdx = i; break; }
    }
    if (targetChain < 0 && targetSynth == ModuleType::LuxSynth)   // LuxWave shares the slot
        for (int c = 0; c < chainModel_.numChains() && targetChain < 0; ++c)
        {
            const auto& mods = chainModel_.chains[(size_t) c].modules;
            for (int i = 0; i < (int) mods.size(); ++i)
                if (mods[i].type == ModuleType::LuxWave) { targetChain = c; synthIdx = i; break; }
        }
    if (targetChain < 0) { targetChain = 0; synthIdx = (int) chainModel_.chains[0].modules.size(); }

    // 2) Ensure exactly one VideoScroll exists, BEFORE the synth (executor only
    //    emits inserts UPSTREAM of the synth — a downstream probe never captures).
    bool hasVideoScroll = false;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll) { hasVideoScroll = true; break; }

    if (! hasVideoScroll)
    {
        // 3-arg signature: insert(chainIdx, type, dropIdx). dropIdx == synthIdx
        // drops the probe immediately before the synth.
        if (! chainModel_.insert(targetChain, ModuleType::VideoScroll, synthIdx))
            log_warn("VST", "VideoScroll migration insert rejected (pool/rule).");
    }

    // 3) ALWAYS copy legacy values into the slot-0 bank this pass (decoupled from
    //    the presence check — a legacy reload with no CHAINS child still carries
    //    real legacy param values that must transfer). Force slot 0 for the copy.
    chainModel_.validateAndRepair();   // assigns/heals slots; first instance → 0
    for (auto& ch : chainModel_.chains)
        for (auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll && m.slot != 0)
            { /* heal-only repair keeps the migrated instance at 0 if it is the
                 only one; if not, the copy targets whatever instance is on slot 0 */ }

    auto copy = [this](const char* legacyId, const char* bankId)
    {
        auto* src = apvts.getParameter(legacyId);
        auto* dst = apvts.getParameter(bankId);
        if (src && dst) dst->setValueNotifyingHost(src->getValue());  // normalised-domain
    };
    copy("videoScrollSpeed",         "videoScroll0_speed");
    copy("videoScrollLinePos",       "videoScroll0_linePos");
    copy("videoScrollMode",          "videoScroll0_mode");
    copy("videoScrollLineThickness", "videoScroll0_thickness");
    copy("videoScrollZoom",          "videoScroll0_zoom");
    copy("videoScrollFade",          "videoScroll0_fade");
    copy("videoScrollMaxDuration",   "videoScroll0_compress");
    copy("videoInvertColor",         "videoScroll0_invert");
    copy("videoColorMode",           "videoScroll0_colorMode");
    if (auto* dstPaused = apvts.getParameter("videoScroll0_paused"))
        dstPaused->setValueNotifyingHost(1.0f);   // open stopped (matches setStateInformation)

    // 4) Re-derive routing + persist.
    chainActiveTypes_.clear();
    chainModel_.deriveActiveTypes(chainActiveTypes_);
    deriveChainRouting();
    persistChainModel();
    state.setProperty("videoScrollMigratedV1", 1, nullptr);

    log_info("VST", "Migrated legacy Video Scroll → slot 0 on chain %d (legacy source %d).",
             targetChain, srcSel);
}
```

> **Value-copy domain:** normalised (`getValue()/setValueNotifyingHost`) — correct for choice/bool/float regardless of range. The `compress` bank uses the identical `1..64` range (registered in PR2) so the squish amount maps 1:1, but correctness does not depend on it. The earlier "raw value / identical ranges asserted" claim is dropped.

**Call sites** — constructor (after `loadChainModelFromState()` ~line 1190):
```cpp
    loadChainModelFromState();
    migrateLegacyVideoScroll();   // cold makeDefault stamps the guard; legacy reload copies values
```
and `setStateInformation` (after `loadChainModelFromState()` ~line 2153):
```cpp
    loadChainModelFromState();
    migrateLegacyVideoScroll();
```

> The `setStateInformation` legacy `videoScrollPaused=1` + `requestVideoScrollClear()` (~line 2136) stay this release (operating on the legacy id / global gen, harmless). They are removed together with the global atomic below.

## Cleanup
- **Delete** the dead global clear plumbing: `videoScrollClearGen`, `getVideoScrollClearGen()`, `requestVideoScrollClear()` (PluginProcessor.h ~80–81/367–369) and their `setStateInformation` call (~line 2136) — no live reader remains after PR3.
- **Delete** `ui/WaterfallColumnComponent.cpp/.h` only if its role is fully replaced by the mixer hosted directly in the editor; if the column wrapper is retained as the mixer host, keep it. (This spec retains the column as the mixer host, so the file stays; its CMake lines stay.) If deleted, remove CMake lines 77–78 and rewrite the ~10 `layoutZones` call-sites to size/place the mixer-hosting component instead.
- **Keep** all legacy `videoScroll*` params registered (hidden) this release for the migration read; schedule deletion for the next release together with the guard.
- Grep gate (must be empty except `createParameterLayout` legacy block and the migration reader):
```
grep -rn "videoScrollSource\b\|getVideoScrollClearGen\|requestVideoScrollClear\|CaptureThread" vst/source
```

## PR4 verification
- Load an **old session with no CHAINS child** and non-default legacy values → after load, one VideoScroll on slot 0 before the synth, and `videoScroll0_speed/zoom/fade/mode/...` equal the legacy values (normalised parity). Guard prop set to 1.
- Re-save and reload → migration does **not** run again (guard set); user edits to slot-0 bank are preserved (no overwrite).
- Fresh install (`makeDefault`) → guard stamped, slot-0 probe present, waterfall captures and renders in the mixer.
- Confirm no dangling references to the removed global clear API (build clean; grep gate empty).

---

## Cross-PR file/edit index

| PR | New files | Edited files |
|---|---|---|
| PR1 | `processing/video_scroll.{h,c}` | `image_chain.{h,c}`, `chain_plan.h`, `ModuleCatalog.h`, `ChainModel.{h,cpp}`, `multithreading.c`, `PluginProcessor.cpp` (init, plan), `CMakeLists.txt` |
| PR2 | — | `PluginProcessor.{h,cpp}` (layout, helpers, pause, activeVideoSlots), `ChainRackComponent.{h,cpp}`, `ModuleCatalogComponent.cpp`, `VideoScrollTab.{h,cpp}`, `PluginEditor.{h,cpp}` |
| PR3 | `ui/VideoMixerComponent.{h,cpp}` | `VideoDisplayComponent.{h,cpp}`, `VideoWindow.{h,cpp}`, `WaterfallColumnComponent.{h,cpp}`, `PluginEditor.cpp`, `CMakeLists.txt` |
| PR4 | — | `PluginProcessor.{h,cpp}` (migration, dead-code removal), `CMakeLists.txt` (conditional) |

---

# Pre-Implementation Correction Addendum (final adversarial pass vs real files)

> Verified against the actual current sources. These corrections SUPERSEDE the body where they conflict. 4 blockers / 3 majors / small fixes. Apply before coding.

## BLOCKERS

**A1. The catalogue panel is HEADER-ONLY — edit `ui/ModuleCatalogComponent.h`, not a `.cpp`.**
`ui/ModuleCatalogComponent.cpp` does not exist (CMake lists only the `.h`). The real section list is `order[]` at **ModuleCatalogComponent.h:63-64**:
```cpp
const ModuleCat order[] = { ModuleCat::SRC, ModuleCat::MIDI,
                            ModuleCat::UTILS, ModuleCat::SYNTH };
```
→ Append `ModuleCat::VIDEO` to `order[]`. The chip list (ctor ~L33-38) iterates `moduleTable()` and filters by `moduleCategory()` in `resized()` (~L71), so the VIDEO SCROLL chip auto-appears once the table row + `order[]` entry exist.

**A2. Bump the hardcoded section count in `ModuleCatalogComponent::preferredHeight()`.**
Line ~83: `return kTopPad + 4 * (kHeaderH + 2 + kSecGap) + ...` — the `4` must become `5` (or `(int) std::size(order)`), else the palette clips one section.

**A3. `layoutZone3()` has TWO exhaustive-no-default switches — handle BOTH.**
The spec body adds a `VideoScroll` case only to the PLAY-face switch (~L678-700). There is also a SETUP-face switch (~L655-674). Add:
```cpp
case ChainBlockId::VideoScroll: break;   // no SETUP face
```
to the SETUP-face switch too, or it `-Wswitch`-breaks under `-Werror`.

**A4. PR2 is NOT independently buildable — land PR2+PR3 column edits together.**
PR2 deletes `VideoScrollTab`'s window API (`toggleDetachedWindow`/`requestFullscreenWindow`/`isVideoWindowOpen`/`openDetachedWindow`/`onWindowStateChanged`/`videoWindow_`), but `WaterfallColumnComponent.cpp` (a PR3 file) calls all of them at ~L204-263. Either fold the WaterfallColumnComponent rewrite into PR2, or explicitly sequence PR2+PR3 as one build unit. Update the "PR1/PR2 independently buildable" claim.

## MAJORS

**A5. `VideoWindow` ownership: `content_` is a RAW pointer owned via `setContentOwned(...)`.**
Real `VideoWindow.h:69` + `.cpp:117-119`. The body's rewrite implying `std::unique_ptr<ContentComponent> content_` would DOUBLE-DELETE. Keep `content_` raw + `setContentOwned` (or switch to `setContentNonOwned` if you go unique_ptr — pick one). Also the class actively uses `onCloseRequested` (L34), the `closeButtonPressed()` override (L29; `ContentComponent` ctor calls `owner_.closeButtonPressed()` at `.cpp:33`), and the `fullscreenBtn_`/`closeBtn_` toolbar — the rewrite must explicitly retain or rewrite these, not drop them. The `display` value→`unique_ptr` conversion + `getDisplay()` returning `content_->display.get()` is correct (display ctor now needs `(proc, slot)`).

**A6. The `WaterfallColumnComponent` rewrite is much larger than the body implies.**
Real `.h` 149 / `.cpp` 487 lines. Replacing `videoTab` with the mixer requires removing: 5 APVTS attachments (`videoScrollPaused` ~L269/283, `videoInvertColor` ~L297, `videoColorMode` ~L303), the `doStop` lambda (`requestVideoScrollClear()` ~L251), the play/pause/stop/invert/colorMode buttons + their grip copies and `setCollapsed()` visibility toggles (~L319-331), and the `onWindowStateChanged`/`windowOpen` machinery (~L124, L204-209). Keep collapse/expand. Enumerate all of this in PR3.

**A7. `deriveAndPublishChainPlan` fill-loop anchor drifted +48 lines.**
The real upstream loop is at **PluginProcessor.cpp:2481-2494** (not ~2433). Structural anchor: the `for (int i = 0; i < idx; ++i)` immediately after `const int stateIdx = ...;` (~L2479) inside the `fill` lambda. (The body's BEFORE/AFTER text matches the real code.) Likewise: lux init is at **~1182-1183** (include goes INSIDE the `extern "C"` block ~L5-21); the LuxMask config-sync site is at **~2849** (insert pause-sync before the trailing `log_debug`); `loadChainModelFromState` migration site is at **~2153**. Use structural anchors, not the body's literal line numbers.

## SMALLER FIXES

**A8. `blockHasSetup` has NO switch — it `return true;` unconditionally (~L203-208).** To make VideoScroll setup-less, change it to branch, e.g. `return id != ChainBlockId::VideoScroll;` (the body's "add a case" is not applicable). The power-button still auto-hides via the empty `enableParamId`.

**A9. CMake placement:** add `ui/VideoMixerComponent.cpp/.h` to the `ui/` `target_sources` block (~L72-85), beside the other M4/M6 components. `WaterfallColumnComponent.cpp/.h` removal at L77-78 and `video_scroll.c` add near the `lux_pitch.c/lux_mask.c` lines (~L228-229) are confirmed correct.

## VERIFIED OK (no change needed)
- `selectBlock` PLAY switch (PluginEditor.cpp:259-298, no default) — body's `case ChainBlockId::VideoScroll` is correct.
- `chainBlockToModuleType` (L33-48) and `instanceToBlockId` (L335-353) both have `default:` — adding the explicit VideoScroll case before the default is correct and required for routing.
- `applyZone3Visibility` (L211-239) is an if-chain — body's added `setVisible(play && id == ChainBlockId::VideoScroll)` fits.
- `onModelChanged` lambda at PluginEditor.cpp:42 — PR3 superset rewrite is clean.
- `moduleCatLabel` + `descFor` ordinal/index invariant — appending enum + table row LAST keeps `index == ordinal`; `ModuleDrag::moduleType` bounds-check grows to 11 automatically.
