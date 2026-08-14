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

#ifdef _WIN32
#define PACKED_STRUCT __declspec(align(4))
#else
#define PACKED_STRUCT __attribute__((aligned(4)))
#endif


//---------------------------------------------------------------------------------------------------------------------------------------------------------
//  COMMON STRUCTURE CIS / MAX
//---------------------------------------------------------------------------------------------------------------------------------------------------------

typedef enum
{
    SW1  = 0,
    SW2,
    SW3,
}buttonIdTypeDef;

typedef enum
{
    SWITCH_RELEASED = 0,
    SWITCH_PRESSED
}buttonStateTypeDef;

typedef enum
{
    LED_1 = 0,
    LED_2,
    LED_3,
}ledIdTypeDef;

typedef enum
{
    STARTUP_INFO_HEADER = 0x11,
    IMAGE_DATA_HEADER = 0x12,
    IMU_DATA_HEADER = 0x13,
    BUTTON_DATA_HEADER= 0x14,
    LED_DATA_HEADER = 0x15,
}CIS_Packet_HeaderTypeDef;

typedef enum
{
    IMAGE_COLOR_R = 0,
    IMAGE_COLOR_G,
    IMAGE_COLOR_B,
}CIS_Packet_ImageColorTypeDef;

typedef enum
{
    CIS_CAL_REQUESTED = 0,
    CIS_CAL_START,
    CIS_CAL_PLACE_ON_WHITE,
    CIS_CAL_PLACE_ON_BLACK,
    CIS_CAL_EXTRACT_INNACTIVE_REF,
    CIS_CAL_EXTRACT_EXTREMUMS,
    CIS_CAL_EXTRACT_OFFSETS,
    CIS_CAL_COMPUTE_GAINS,
    CIS_CAL_END,
}CIS_Calibration_StateTypeDef;

// Packet header structure defining the common header for all packet types// Structure for packets containing startup information like version info
struct PACKED_STRUCT packet_StartupInfo
{
    CIS_Packet_HeaderTypeDef type;         // Identifies the data type
    uint32_t packet_id;                   // Sequence number, useful for ordering packets
    uint8_t version_info[32];             // Information about the version, and other startup details
};

// Structure for image data packets, including metadata for image fragmentation
struct PACKED_STRUCT packet_Image
{
    CIS_Packet_HeaderTypeDef type;                     // Identifies the data type
    uint32_t packet_id;                               // Sequence number, useful for ordering packets
    uint32_t line_id;                                  // Line identifier
    uint8_t fragment_id;                              // Fragment position
    uint8_t total_fragments;                          // Total number of fragments for the complete image
    uint16_t fragment_size;                           // Size of this particular fragment
    uint8_t imageData_R[UDP_LINE_FRAGMENT_SIZE];       // Pointer to the fragmented red image data
    uint8_t imageData_G[UDP_LINE_FRAGMENT_SIZE];      // Pointer to the fragmented green image data
    uint8_t imageData_B[UDP_LINE_FRAGMENT_SIZE];    // Pointer to the fragmented blue image data
};

struct PACKED_STRUCT button_State
{
    buttonStateTypeDef state;
    uint32_t pressed_time;
};

// Structure for packets containing button state information
struct PACKED_STRUCT packet_Button
{
    CIS_Packet_HeaderTypeDef type;                         // Identifies the data type
    uint32_t packet_id;                   // Sequence number, useful for ordering packets
    buttonIdTypeDef button_id;                 // Id of the button
    struct button_State button_state;         // State of the led A
};

struct PACKED_STRUCT led_State
{
    uint16_t brightness_1;
    uint16_t time_1;
    uint16_t glide_1;
    uint16_t brightness_2;
    uint16_t time_2;
    uint16_t glide_2;
    uint32_t blink_count;
};

// Structure for packets containing leds state
struct PACKED_STRUCT packet_Leds
{
    CIS_Packet_HeaderTypeDef type;                         // Identifies the data type
    uint32_t packet_id;                   // Sequence number, useful for ordering packets
    ledIdTypeDef led_id;                 // Id of the led
    struct led_State led_state;         // State of the selected led
};

// Structure for packets containing sensor data (accelerometer and gyroscope)
struct PACKED_STRUCT packet_IMU
{
    CIS_Packet_HeaderTypeDef type;                         // Identifies the data type
    uint32_t packet_id;                   // Sequence number, useful for ordering packets
    float_t acc[3];                   // Accelerometer data: x, y, and z axis
    float_t gyro[3];                  // Gyroscope data: x, y, and z axis
    float_t integrated_acc[3];        // Accelerometer data: x, y, and z axis
    float_t integrated_gyro[3];       // Gyroscope data: x, y, and z axis
};

struct PACKED_STRUCT cisRgbBuffers
{
    uint8_t *R;  // Dynamic allocation
    uint8_t *G;  // Dynamic allocation
    uint8_t *B;  // Dynamic allocation
};

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
