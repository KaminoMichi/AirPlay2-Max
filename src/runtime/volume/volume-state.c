#include "volume-state.h"
/* Pure policy; caller serializes it. Indices are Android UI steps, not dB. */
void yv_begin(ya_volume_state*s,int cid,int64_t now){
 s->active=1;s->cid=cid;s->fault=s->inflight||s->notifying;s->first=1;s->pending=0;s->mute_requested=0;s->epoch++;
 s->guard_until=now+2000;s->notice++;
 for(int i=0;i<=100;i++)s->echo_until[i]=0;
}
void yv_end(ya_volume_state*s){s->active=0;s->pending=0;s->mute_requested=0;s->epoch++;}
int yv_request(ya_volume_state*s,int idx,int initial,int64_t now){
 if(idx<0||idx>100||!s->active||!s->ready||s->fault||now-s->read_at>1000)return 0;
 /* Initial player synchronisation is a read-only reflection of Android. */
 if(initial){s->notice++;return 0;}
 int first=s->first;s->first=0;
 if(idx==0){
  /* A real mute outranks startup/echo suppression. Publish silence before IPC.
   * A later nonzero request may replace target, but cannot erase this intent. */
  s->mute_requested=1;
 }else{
  if(first||now<s->guard_until){s->notice++;return 0;}
  if((idx==s->current&&!s->pending&&!s->mute_requested)||now<s->echo_until[idx])return 0;
 }
 if(!s->pending)s->due=now+60;
 s->target=idx;s->request++;s->pending=1;return 1;
}
void yv_observe(ya_volume_state*s,int idx,uint64_t epoch,uint64_t request,int64_t now){
 if(idx<0||idx>100){s->ready=0;s->fault=s->active;return;}
 int changed=!s->ready||idx!=s->current;s->ready=1;s->current=idx;s->read_at=now;
 if(epoch==s->epoch&&request==s->request&&request){
  /* Wire parser checks this too; don't let a mismatched ACK release mute. */
  if(idx!=s->target){s->ready=0;s->fault=s->active;return;}
  s->applied=request;s->pending=0;s->mute_requested=0;
 }
 if(changed&&(!request||epoch!=s->epoch))s->notice++;
}
int yv_gate(ya_volume_state*s,int64_t now){
 if(!s->active)return -1;
 if(!s->ready||s->fault||now-s->read_at>1000){s->fault=1;return -1;}
 /* Ordinary pending SET leaves PCM continuous at the last trusted system level.
  * Only actual mute or unacknowledged mute intent substitutes zero samples. */
 return s->mute_requested||s->current==0?1:0;
}
