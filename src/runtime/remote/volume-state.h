#pragma once
#include <stdint.h>
typedef struct {
 int ready,current,active,cid,fault,first,pending,target,inflight,notifying,mute_requested;
 uint64_t epoch,request,applied,notice;
 int64_t read_at,guard_until,due,echo_until[101];
} ya_volume_state;
void yv_begin(ya_volume_state*,int,int64_t);
void yv_end(ya_volume_state*);
int yv_request(ya_volume_state*,int,int,int64_t);
void yv_observe(ya_volume_state*,int,uint64_t,uint64_t,int64_t);
int yv_gate(ya_volume_state*,int64_t);
