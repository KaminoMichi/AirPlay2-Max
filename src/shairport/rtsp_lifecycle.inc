/* Included by rtsp.c and the host contention test. All table ownership lives
 * under conns_lock. A claimed entry remains discoverable until join completes.
 * No reference escapes this module except principal_conn, released by worker
 * cleanup before publishing running=0. Callers must not hold principal_conn_lock.
 */
static pthread_cond_t conns_changed = PTHREAD_COND_INITIALIZER;
static int reap_connection(int id, int request_stop) {
 int old;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old);
 pthread_mutex_lock(&conns_lock);
 rtsp_conn_info *target=NULL;
 for (;;) {
  target=NULL;
  for(int i=0;i<nconns;i++)if(conns[i]&&conns[i]->connection_number==id){target=conns[i];break;}
  if(!target||pthread_equal(target->thread,pthread_self())){target=NULL;break;}
  if(!target->rtsp_reaping){
   if(request_stop)pthread_cancel(target->thread);
   target->rtsp_reaping=1;break;
  }
  pthread_cond_wait(&conns_changed,&conns_lock);
 }
 pthread_mutex_unlock(&conns_lock);
 int joined=0;
 if(target){
  int rc=pthread_join(target->thread,NULL);
  pthread_mutex_lock(&conns_lock);
  if(rc==0){
   for(int i=0;i<nconns;i++)if(conns[i]==target){conns[i]=NULL;break;}
   joined=1;
  }else{target->rtsp_reaping=0;}
  pthread_cond_broadcast(&conns_changed);
  pthread_mutex_unlock(&conns_lock);
  if(joined)free(target);else debug(1,"RTSP join failed: %d",rc);
 }
 pthread_setcancelstate(old,NULL);return joined;
}
void cancel_all_RTSP_threads(airplay_stream_c category,int except_id){
 int old;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&old);
 pthread_mutex_lock(&conns_lock);
 int *ids=nconns?calloc((size_t)nconns,sizeof(int)):NULL;int count=0;
 if(nconns&&!ids){pthread_mutex_unlock(&conns_lock);die("RTSP stop list allocation failed");}
 for(int i=0;i<nconns;i++){
  rtsp_conn_info*c=conns[i];
  if(c&&c->connection_number!=except_id&&!pthread_equal(c->thread,pthread_self())&&
    (category==unspecified_stream_category||c->airplay_stream_category==category)){
   ids[count++]=c->connection_number;pthread_cancel(c->thread);
  }
 }
 pthread_mutex_unlock(&conns_lock);
 for(int i=0;i<count;i++)reap_connection(ids[i],1);
 free(ids);pthread_setcancelstate(old,NULL);
}
int old_connection_count=-1;
void cleanup_threads(void){
 for(;;){
  int id=0;pthread_mutex_lock(&conns_lock);
  for(int i=0;i<nconns;i++)if(conns[i]&&!conns[i]->rtsp_reaping&&conns[i]->running==0){id=conns[i]->connection_number;break;}
  pthread_mutex_unlock(&conns_lock);
  if(!id)break;
  reap_connection(id,0);
 }
}
int terminate_conn(int id){return reap_connection(id,1);}
