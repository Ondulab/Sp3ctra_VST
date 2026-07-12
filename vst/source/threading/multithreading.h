#ifndef MULTITHREADING_H
#define MULTITHREADING_H

#include <pthread.h>
#include "config.h"
#include "doublebuffer.h"

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <stdint.h>
typedef SSIZE_T ssize_t;
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

/* Synth-split P3 — FramePlayerThread: stage every PLAYER-OWNED LuxStral send
 * from the blended playback frame; each send applies its own post-marker
 * inserts + bank on a private copy. Per-chain playback: a sampler send is
 * player-owned only when its chain hosts THIS engine (`engine_slot` = the
 * calling player's engine); is_score=1 matches has_score chains instead.
 * Returns the plan's num_ls_sends (0 → caller keeps the legacy engine-A
 * player path alive). VST only. */
struct AudioImageBuffers;
int ls_sends_stage_player_frame(const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels,
                                int is_score, int engine_slot, int force_play,
                                struct AudioImageBuffers *viz_bus);

/* Engine A ← player-side chain inserts. Called by FramePlayerThread (Non-RT)
 * with the final blended playback frame: applies IN PLACE the inserts of
 * LuxStral A's chain placed BELOW the SCORE (is_score=1) / SAMPLER (is_score=0,
 * marker matching `engine_slot`) module — REVERB/ECHO/probes — and publishes
 * the zone-1 selection tap when it points into that span. The per-line
 * producers skip those inserts while the player owns the channel, so this is
 * their only execution. */
void chain_player_apply_synth_a_inserts(int is_score, int engine_slot,
                                        struct AudioImageBuffers *viz_bus,
                                        uint8_t *r, uint8_t *g, uint8_t *b,
                                        int nb_pixels);

/* M4 — FramePlayerThread: stage the "→ LUXSYNTH" send from the blended
 * playback frame while the player owns its chain's stream (single writer —
 * udpThread/feeder skip the LuxSynth staging of player-owned chains).
 * Ownership: has_score (is_score=1) or SAMPLER marker == engine_slot. */
void lx_send_stage_player_frame(const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels,
                                int is_score, int engine_slot);

/* M7 — plan-driven ownership queries (replace the legacy *_source_type
 * gates in the player paths). Non-RT callers. Per-chain playback: the
 * sampler case (is_score=0) matches the chain's SAMPLER marker against
 * `engine_slot`; the score case (is_score=1) ignores it. */
int chain_additive_player_candidate(int is_score, int engine_slot);
int chain_pathb_player_candidate(int is_score, int engine_slot);

#endif
