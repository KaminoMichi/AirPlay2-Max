#pragma once
#include <stdint.h>
/* Only the player startup thread sets this; callbacks consume no blocking IPC. */
extern _Thread_local int ya_rc14_initial_volume;
int ya_volume_init(void);
void ya_volume_deinit(void);
void ya_volume_begin(int cid);
void ya_volume_end(void);
void ya_volume_request(int index);
double ya_volume_initial_db(void);
/* 0 permits PCM;1 sends bounded zero buffers until ACK;-1 closes the session. */
int ya_volume_gate(void);
int ya_volume_notify(int cid,double unit);

extern _Thread_local int ya_rc14_notice_index;
void ya_volume_mark_echo(void);
