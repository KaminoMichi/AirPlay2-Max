#define _POSIX_C_SOURCE 200809L
#include "diag-audio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include <stddef.h>
_Static_assert(ATOMIC_LLONG_LOCK_FREE==2,"diagnostic counters require lock-free 64-bit atomics");
static atomic_ullong counters[D_COUNT],last_end,max_gap,max_lock,max_api;
static atomic_llong gauges[G_COUNT];
static const char *const names[D_COUNT]={
 "calls","requested_frames","write_calls","write_requested","written_frames",
 "write_zero","write_partial","write_errors","gap_gt20ms","gap_gt40ms","gap_gt80ms",
 "gap_gt150ms","timestamp_calls","timestamp_errors","negative_delay",
 "rtp_packets","rtp_bytes","rtp_bad","control_packets","player_packets",
 "late","too_late","missing","resends","clock_errors","configures","flushes"};
static const char *const gnames[G_COUNT]={"generation","active","rate","api_mask","xrun","buffer_size","capacity","burst","delay_frames","player_occupancy","last_write_error"};
static void peak(atomic_ullong *p,uint64_t value){
 unsigned long long old=atomic_load_explicit(p,memory_order_relaxed);
 while(old<value&&!atomic_compare_exchange_weak_explicit(p,&old,value,memory_order_relaxed,memory_order_relaxed)){}
}
void ya_diag_init(void){
 for(unsigned i=0;i<D_COUNT;i++)atomic_store(&counters[i],0);
 for(unsigned i=0;i<G_COUNT;i++)atomic_store(&gauges[i],-1);
 atomic_store(&gauges[G_GENERATION],0);atomic_store(&gauges[G_ACTIVE],0);
 atomic_store(&last_end,0);atomic_store(&max_gap,0);atomic_store(&max_lock,0);atomic_store(&max_api,0);
}
void ya_diag_add(enum ya_dm id,uint64_t n){if((unsigned)id<D_COUNT)atomic_fetch_add_explicit(&counters[id],n,memory_order_relaxed);}
void ya_diag_set(enum ya_dg id,int64_t n){if((unsigned)id<G_COUNT)atomic_store_explicit(&gauges[id],n,memory_order_relaxed);}
uint64_t ya_diag_counter(enum ya_dm id){return (unsigned)id<D_COUNT?atomic_load_explicit(&counters[id],memory_order_relaxed):0;}
int64_t ya_diag_gauge(enum ya_dg id){return (unsigned)id<G_COUNT?atomic_load_explicit(&gauges[id],memory_order_relaxed):-1;}
uint64_t ya_diag_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t)){ya_diag_add(D_CLOCK_ERRORS,1);return 0;}return (uint64_t)t.tv_sec*1000000000ULL+(uint64_t)t.tv_nsec;}
void ya_diag_stream(unsigned rate,int result){
 ya_diag_add(D_STREAM_CONFIGURES,1);atomic_fetch_add_explicit(&gauges[G_GENERATION],1,memory_order_relaxed);
 ya_diag_set(G_RATE,rate);ya_diag_set(G_ACTIVE,result==0);atomic_store(&last_end,0);
 for(unsigned i=G_XRUN;i<=G_BURST;i++)ya_diag_set((enum ya_dg)i,-1);
}
void ya_diag_write(int requested,int result,uint64_t lock_start,uint64_t api_start,uint64_t api_end){
 ya_diag_add(D_WRITE_CALLS,1);if(requested>0)ya_diag_add(D_WRITE_REQUEST,(unsigned)requested);
 if(api_start>=lock_start&&lock_start)peak(&max_lock,api_start-lock_start);
 if(api_end>=api_start&&api_start)peak(&max_api,api_end-api_start);
 if(result<0){ya_diag_add(D_WRITE_ERRORS,1);ya_diag_set(G_LAST_WRITE_ERROR,result);}
 else if(result==0)ya_diag_add(D_WRITE_ZERO,1);
 else{
  ya_diag_add(D_WRITE_FRAMES,(unsigned)result);if(result<requested)ya_diag_add(D_WRITE_PARTIAL,1);
  uint64_t previous=atomic_exchange_explicit(&last_end,api_end,memory_order_relaxed);
  if(previous&&api_start>=previous){uint64_t gap=api_start-previous;peak(&max_gap,gap);
   if(gap>20000000ULL)ya_diag_add(D_GAP_20MS,1);
   if(gap>40000000ULL)ya_diag_add(D_GAP_40MS,1);
   if(gap>80000000ULL)ya_diag_add(D_GAP_80MS,1);
   if(gap>150000000ULL)ya_diag_add(D_GAP_150MS,1);
  }
 }
}
void ya_diag_report(void){
 char b[4096];int n=snprintf(b,sizeof b,"YA_AUDIO_DIAG v=1 monotonic_ns=%" PRIu64,ya_diag_now());
 if(n<0||(size_t)n>=sizeof b)return;
 size_t used=(size_t)n;
 for(unsigned kind=0;kind<2;kind++)for(unsigned i=0;i<(kind?G_COUNT:D_COUNT);i++){
  if(kind)n=snprintf(b+used,sizeof b-used," %s=%" PRId64,gnames[i],ya_diag_gauge((enum ya_dg)i));
  else n=snprintf(b+used,sizeof b-used," %s=%" PRIu64,names[i],ya_diag_counter((enum ya_dm)i));
  if(n<0||(size_t)n>=sizeof b-used){return;}
  used+=(size_t)n;
 }
 n=snprintf(b+used,sizeof b-used," max_gap_ns=%llu max_lock_ns=%llu max_api_ns=%llu\n",atomic_load(&max_gap),atomic_load(&max_lock),atomic_load(&max_api));
 if(n>0&&(size_t)n<sizeof b-used)fputs(b,stderr);
}
