#ifndef MULTITHREADING_H
#define MULTITHREADING_H

#include <pthread.h>
#include "config.h"
#include "doublebuffer.h"
#include "../processing/chain_plan.h"

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <stdint.h>
#include <math.h> /* float_t (packet_IMU) */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include "math.h"
#endif

/* The CIS wire format is the Sp3ctra Link contract (communication/link/sp3ctra_link.h);
 * the legacy packet_* structs and CIS_Packet_HeaderTypeDef are gone (firmware >= 4.0). */

//---------------------------------------------------------------------------------------------------------------------------------------------------------
// PROTOTYPES
//---------------------------------------------------------------------------------------------------------------------------------------------------------

int initDoubleBuffer(DoubleBuffer *db);   // 0 = ok, -1 = init/alloc failure
void swapBuffers(DoubleBuffer *db);
void *udpThread(void *arg);
void *audioProcessingThread(void *arg);

/* M9 — one feeder tick: drives the per-synth chains from the IMAGE/VIDEO/
 * CAMERA internal sources while the SP3CTRA device is NOT streaming (when it
 * streams, udpThread substitutes the source frames itself, at line rate).
 * Called by MediaSourceService (Non-RT JUCE thread) at a few hundred Hz.
 * `arg` is the Context*. No-op when no internal source is active. */
void internal_sources_process_tick(void *arg);

/* ── P4-M2 — FramePlayerThread: ONE positional walk per player-owned chain ──
 * For every chain owned by THIS player (its driving engine's SAMPLER marker,
 * or the SCORE-type marker during score playback — is_score=1), walks the
 * span BELOW the owning marker on the blended playback frame with the SAME
 * executor as udpThread/feeder: stages every OUT (LuxStral/LuxSynth/LuxWave)
 * at its exact position, runs post-marker FX/probes exactly once, records the
 * downstream SAMPLER markers (bounce/resampling — never the driving engine)
 * and publishes the exact zone-1 selection tap. The stream at the first owned
 * LuxStral OUT is copied back into r/g/b (display mix bus + legacy commits
 * see post-FX) and published as engine tap A. Returns plan.num_ls_sends
 * (0 → caller keeps the legacy engine-A player path alive). VST only. */
struct AudioImageBuffers;
int chain_player_execute_owned(int is_score, int engine_slot, int force_play,
                               struct AudioImageBuffers *viz_bus,
                               uint8_t *r, uint8_t *g, uint8_t *b,
                               int nb_pixels);

/* M7 — plan-driven ownership queries (replace the legacy *_source_type
 * gates in the player paths). Non-RT callers. Per-chain playback: the
 * sampler case (is_score=0) matches the chain's SAMPLER marker against
 * `engine_slot`; the score case (is_score=1) ignores it. */
int chain_additive_player_candidate(int is_score, int engine_slot);
int chain_pathb_player_candidate(int is_score, int engine_slot);

/* 1 while THIS player owns at least one present chain's stream — hosts a
 * marker NOT masked by a feeding source placed below it (order is the law
 * for sources too, 2026-08-20). The players' visual-bus claims must ride on
 * it: a playing slot whose markers all sit above a feeding source injects
 * nowhere and leaves the display to the producers. Non-RT. */
int chain_player_owns_any_stream(int is_score, int engine_slot);

/* Player stop → staging silence: deactivate the LuxStral/LuxSynth/LuxWave
 * stagings of every chain owned by THIS player. The stagings have no
 * timeout — without this, a stopped player on a sourceless chain leaves its
 * last column ringing forever. Non-RT.
 *   chain_player_stagings_set_inactive — sampler engines (FramePlayerThread::
 *     injectWhiteFrame), SAMPLER-marker ownership only since P5-M4;
 *   score_player_stagings_set_inactive — score-player slots
 *     (ScorePlayerService session teardown), SCORE-marker ownership. */
void chain_player_stagings_set_inactive(int engine_slot);
void score_player_stagings_set_inactive(int score_slot);

/* Player stop → downstream blend-reference silence: whiten the MIX/darken-
 * blend input cache of every SAMPLER marker BELOW the stopping player's own
 * marker in its owned chains. Companion of the staging deactivation — without
 * it, a downstream sampler keeps blending the stopped player's LAST column
 * (e.g. a VOICE head position) into its playback. Same split as above:
 *   chain_player_whiten_downstream_inputs — sampler engines;
 *   score_player_whiten_downstream_inputs — score-player slots. Non-RT. */
void chain_player_whiten_downstream_inputs(int engine_slot);
void score_player_whiten_downstream_inputs(int score_slot);

/* ── FX tail runout ──────────────────────────────────────────────────────────
 * Reverb/Echo keep printing after their input goes silent — that IS the
 * module. A chain whose feed stops must therefore keep being walked on blank
 * paper until its tails are spent, or the decay is truncated on the spot.
 *
 *   chain_player_fx_tail_alive — 1 while a chain owned by THIS player still
 *     has a tail BELOW its owning marker. A stopping player keeps its session
 *     alive and injects blank paper while this holds (ScorePlayerService), so
 *     the decay runs at the player's own line rate. VST only.
 *   chain_any_fx_tail_alive — pool-wide, no plan needed: lets the media source
 *     feeder stay at source rate through a runout instead of dropping to its
 *     20 Hz idle poll.
 * The producers' own runout is internal (chain_span_fx_tail_alive). Non-RT. */
int chain_player_fx_tail_alive(int is_score, int engine_slot);
int chain_any_fx_tail_alive(void);

#endif
