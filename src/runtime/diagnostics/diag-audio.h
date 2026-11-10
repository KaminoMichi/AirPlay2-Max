#ifndef YA_DIAG_AUDIO_H
#define YA_DIAG_AUDIO_H
#include <stdint.h>
enum ya_dm {
 D_AUDIO_CALLS,D_REQUEST_FRAMES,D_WRITE_CALLS,D_WRITE_REQUEST,D_WRITE_FRAMES,
 D_WRITE_ZERO,D_WRITE_PARTIAL,D_WRITE_ERRORS,D_GAP_20MS,D_GAP_40MS,D_GAP_80MS,
 D_GAP_150MS,D_TIMESTAMP_CALLS,D_TIMESTAMP_ERRORS,D_NEGATIVE_DELAY,
 D_RTP_PACKETS,D_RTP_BYTES,D_RTP_BAD,D_CONTROL_PACKETS,D_PLAYER_PACKETS,
 D_PLAYER_LATE,D_PLAYER_TOO_LATE,D_PLAYER_MISSING,D_RESEND_REQUESTS,D_CLOCK_ERRORS,
 D_STREAM_CONFIGURES,D_STREAM_FLUSHES,D_COUNT
};
enum ya_dg {G_GENERATION,G_ACTIVE,G_RATE,G_API_MASK,G_XRUN,G_BUFFER_SIZE,
 G_CAPACITY,G_BURST,G_DELAY,G_BUFFER_OCCUPANCY,G_LAST_WRITE_ERROR,G_COUNT};
void ya_diag_init(void);
void ya_diag_add(enum ya_dm id,uint64_t n);
void ya_diag_set(enum ya_dg id,int64_t value);
uint64_t ya_diag_counter(enum ya_dm id);
int64_t ya_diag_gauge(enum ya_dg id);
uint64_t ya_diag_now(void);
void ya_diag_stream(unsigned rate,int result);
void ya_diag_write(int requested,int result,uint64_t before_lock,uint64_t before_api,uint64_t after_api);
void ya_diag_report(void);
#endif
