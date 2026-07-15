/**
 * @file score_player_hooks.h
 * @brief P5-M4 — C-linkage queries into the ScorePlayerService (C++).
 *
 * The chain executor gates (multithreading.c) resolve score ownership PER
 * SLOT: a chain's SCORE-family marker carries the instance slot
 * (insert_state_idx), and the chain is player-owned only while THAT slot is
 * feeding. These hooks are the C side of that contract — implemented in
 * ScorePlayerService.cpp against the registered service instance.
 *
 * "Feeding" covers the whole injection window: transport PLAY, scrub
 * audition (held-position injection) and the session teardown tick — any
 * moment the service may still write the chain's stream.
 */
#ifndef SCORE_PLAYER_HOOKS_H
#define SCORE_PLAYER_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/** 1 while score-player slot @p slot (0..7) is feeding its chains. */
int score_player_slot_is_playing(int slot);

/** 1 while ANY score-player slot is feeding (display-bus arbitration:
 *  the sampler engines defer the visual mix bus to the score service,
 *  exactly as they deferred to the legacy shared score channel). */
int score_player_any_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* SCORE_PLAYER_HOOKS_H */
