#pragma once
/* WORK002: input rate and network latency cross decoder/control/player threads.
 * Preserve structure ABI and calculations; use relaxed atomic scalar access.
 * Other codec state retains its existing owning mutex/lifecycle. */
#define YA_INPUT_RATE(c) __atomic_load_n(&(c)->input_rate, __ATOMIC_RELAXED)
#define YA_INPUT_RATE_STORE(c, value) __atomic_store_n(&(c)->input_rate, (value), __ATOMIC_RELAXED)
#define YA_NETWORK_LATENCY(c) __atomic_load_n(&(c)->latency, __ATOMIC_RELAXED)
#define YA_NETWORK_LATENCY_STORE(c, value) __atomic_store_n(&(c)->latency, (value), __ATOMIC_RELAXED)
#define YA_AP2_PLAYING(c) __atomic_load_n(&(c)->ap2_play_enabled, __ATOMIC_RELAXED)
#define YA_AP2_PLAYING_STORE(c, value) __atomic_store_n(&(c)->ap2_play_enabled, (value), __ATOMIC_RELAXED)
