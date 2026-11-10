#include "volume.h"
#include "common.h"
#include "rtsp.h"
#include <errno.h>
/* The upstream notification function builds dvlc but never changes PCM or
 * AudioService. Compiled from the unmodified pinned remote/remote.c. */
extern ssize_t ap2_event_send_unit_volume_notification(rtsp_conn_info *,double);
int ya_volume_notify(int cid,double unit){
 int result=0;
 if(pthread_rwlock_tryrdlock(&principal_conn_lock))return 0;
 pthread_cleanup_push(rwlock_unlock,&principal_conn_lock);
 rtsp_conn_info *conn=principal_conn;
 if(conn&&conn->connection_number==cid&&conn->airplay_type==ap_2){
  struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
  int64_t old=ya_io_set_deadline((int64_t)t.tv_sec*1000000000+t.tv_nsec+500000000);
  ya_rc14_notice_index=(int)lround(unit*100);
  errno=0;ssize_t rc=ap2_event_send_unit_volume_notification(conn,unit);int error=errno;
  ya_rc14_notice_index=-1;ya_io_set_deadline(old);
  result=rc>0?1:(error==EBUSY?0:(error==EOPNOTSUPP?-2:-1));
 }
 pthread_cleanup_pop(1);
 return result;
}
