#pragma once
#include <stdint.h>
#define YR_MAGIC UINT32_C(0x59525431)
#define YR_VERSION 1
#ifndef YR_ROOT
#define YR_ROOT "/data/local/tmp/ya-airplay"
#endif
#define YR_SOCKET YR_ROOT "/run/remote-test.sock"
#ifndef YR_RECEIVER
#define YR_RECEIVER YR_ROOT "/bin/ya-shairport"
#endif
enum { YR_HELLO=1,YR_READY,YR_COMMIT,YR_RESULT };
enum { YR_ACK=0,YR_NO_SESSION,YR_STALE,YR_BUSY,YR_INVALID,YR_DENIED,YR_REJECTED,YR_UNCERTAIN,YR_STOPPING };
/* Local-only same-host fixed-width ABI; no pointers, strings or variable frames. */
typedef struct {
 uint32_t magic,version,type,command,status,cid;
 uint64_t epoch,started,lease,nonce[2];
} yr_packet;
_Static_assert(sizeof(yr_packet)==64,"local IPC ABI");
static inline int yr_command_valid(unsigned c){return c==0||c==1||c==4||c==5;}
static inline const char *yr_command(unsigned c){return c==0?"play":c==1?"pause":c==4?"next":c==5?"previous":"invalid";}
static inline const char *yr_status(unsigned s){switch(s){case YR_ACK:return "rtsp_ack";case YR_NO_SESSION:return "no_session";case YR_STALE:return "stale";case YR_BUSY:return "busy";case YR_INVALID:return "invalid";case YR_DENIED:return "denied";case YR_REJECTED:return "source_rejected";case YR_STOPPING:return "stopping";default:return "uncertain";}}
