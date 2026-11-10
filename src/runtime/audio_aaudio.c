#include "diag-audio.h"
/* Android framework backend derived from the device-tested SF1 implementation.
 * New deployment paths and lifecycle need separate device validation.
 * Shared output; no ALSA or Audio HAL changes. Timing: delay returns FRAMES.
 * RTP timestamp is a sample index, playtime is SPS local ns; neither is an
 * AAudio CLOCK_MONOTONIC timestamp. We report relative delay only; stats=NULL.
 */
#include "audio.h"
#include "common.h"
#include "volume.h"
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <math.h>


static void *lib;
#define API(ret,name,args) static ret (*p_##name) args
API(aaudio_result_t,AAudio_createStreamBuilder,(AAudioStreamBuilder**));
API(aaudio_result_t,AAudioStreamBuilder_delete,(AAudioStreamBuilder*));
API(void,AAudioStreamBuilder_setDirection,(AAudioStreamBuilder*,aaudio_direction_t));
API(void,AAudioStreamBuilder_setSharingMode,(AAudioStreamBuilder*,aaudio_sharing_mode_t));
API(void,AAudioStreamBuilder_setFormat,(AAudioStreamBuilder*,aaudio_format_t));
API(void,AAudioStreamBuilder_setChannelCount,(AAudioStreamBuilder*,int32_t));
API(void,AAudioStreamBuilder_setSampleRate,(AAudioStreamBuilder*,int32_t));
API(aaudio_result_t,AAudioStreamBuilder_openStream,(AAudioStreamBuilder*,AAudioStream**));
API(aaudio_result_t,AAudioStream_close,(AAudioStream*));
API(aaudio_result_t,AAudioStream_requestStart,(AAudioStream*));
API(aaudio_stream_state_t,AAudioStream_getState,(AAudioStream*));
API(aaudio_result_t,AAudioStream_write,(AAudioStream*,const void*,int32_t,int64_t));
API(int32_t,AAudioStream_getSampleRate,(AAudioStream*));
API(int32_t,AAudioStream_getChannelCount,(AAudioStream*));
API(aaudio_format_t,AAudioStream_getFormat,(AAudioStream*));
API(int64_t,AAudioStream_getFramesWritten,(AAudioStream*));
API(int64_t,AAudioStream_getFramesRead,(AAudioStream*));
API(aaudio_result_t,AAudioStream_getTimestamp,(AAudioStream*,clockid_t,int64_t*,int64_t*));
API(int32_t,AAudioStream_getXRunCount,(AAudioStream*));
API(int32_t,AAudioStream_getBufferSizeInFrames,(AAudioStream*));
API(int32_t,AAudioStream_getBufferCapacityInFrames,(AAudioStream*));
API(int32_t,AAudioStream_getFramesPerBurst,(AAudioStream*));
static uint64_t diag_poll_time;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static AAudioStream *stream;
static unsigned rate=48000;
static atomic_int stopping;
/* Never execute a cooperative cancellation point while owning the stream lock. */
static _Thread_local int lock_old;
static void audio_lock(void){pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&lock_old);pthread_mutex_lock(&lock);}
static void audio_unlock(void){int old=lock_old;pthread_mutex_unlock(&lock);pthread_setcancelstate(old,NULL);}
static void close_locked(void){if(stream){p_AAudioStream_close(stream);stream=NULL;ya_diag_set(G_ACTIVE,0);}}
static int open_locked(void){
 AAudioStreamBuilder *b=NULL;int rc=p_AAudio_createStreamBuilder(&b);if(rc)return rc;
 p_AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);
 p_AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);
 p_AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);
 p_AAudioStreamBuilder_setChannelCount(b,2);
 p_AAudioStreamBuilder_setSampleRate(b,rate);
 rc=p_AAudioStreamBuilder_openStream(b,&stream);p_AAudioStreamBuilder_delete(b);
 if(rc)return rc;
 if(p_AAudioStream_getSampleRate(stream)!=(int)rate||p_AAudioStream_getChannelCount(stream)!=2||p_AAudioStream_getFormat(stream)!=AAUDIO_FORMAT_PCM_I16){close_locked();return -EINVAL;}
 rc=p_AAudioStream_requestStart(stream);if(rc)close_locked();return rc;
}
static int init(int argc,char **argv){
 (void)argv;if(argc)return -EINVAL;
 lib=dlopen("libaaudio.so",RTLD_NOW|RTLD_LOCAL);if(!lib)return -ENOSYS;
#define LOAD(name) do { *(void **)(&p_##name)=dlsym(lib,#name);if(!p_##name){dlclose(lib);lib=NULL;return -ENOSYS;} } while(0)
 LOAD(AAudio_createStreamBuilder);LOAD(AAudioStreamBuilder_delete);
 LOAD(AAudioStreamBuilder_setDirection);LOAD(AAudioStreamBuilder_setSharingMode);
 LOAD(AAudioStreamBuilder_setFormat);LOAD(AAudioStreamBuilder_setChannelCount);
 LOAD(AAudioStreamBuilder_setSampleRate);LOAD(AAudioStreamBuilder_openStream);
 LOAD(AAudioStream_close);LOAD(AAudioStream_requestStart);LOAD(AAudioStream_write);
 LOAD(AAudioStream_getState);
 LOAD(AAudioStream_getSampleRate);LOAD(AAudioStream_getChannelCount);LOAD(AAudioStream_getFormat);
 LOAD(AAudioStream_getFramesWritten);LOAD(AAudioStream_getFramesRead);LOAD(AAudioStream_getTimestamp);
 unsigned diag_mask=0;
#define OPTIONAL(name,bit) do { (void)dlerror();*(void **)(&p_##name)=dlsym(lib,#name);if(p_##name)diag_mask|=bit;(void)dlerror(); } while(0)
 OPTIONAL(AAudioStream_getXRunCount,1);OPTIONAL(AAudioStream_getBufferSizeInFrames,2);
 OPTIONAL(AAudioStream_getBufferCapacityInFrames,4);OPTIONAL(AAudioStream_getFramesPerBurst,8);
 ya_diag_set(G_API_MASK,diag_mask);
 parse_audio_options("aaudio",1<<SPS_FORMAT_S16_LE,(1<<SPS_RATE_44100)|(1<<SPS_RATE_48000),1<<2);
 config.audio_backend_buffer_desired_length=0.15;config.audio_backend_latency_offset=0;
 /* Logical AirPlay UI scale, not a measured Android acoustic dB curve. */
 if(config.ignore_volume_control||config.volume_range_db||config.volume_max_db_set||config.loudness_enabled||config.cmd_start||config.cmd_stop||config.cmd_set_volume||config.cmd_unfixable||config.cmd_active_start||config.cmd_active_stop){dlclose(lib);lib=NULL;return -EINVAL;}
 config.volume_control_profile=VCP_flat;
 int volume_rc=ya_volume_init();if(volume_rc){dlclose(lib);lib=NULL;return volume_rc;}
 return 0;
}
static void stop(void){ya_volume_end();atomic_store(&stopping,1);audio_lock();close_locked();audio_unlock();}
static void deinit(void){stop();ya_volume_deinit();if(lib){dlclose(lib);lib=NULL;}}
static int supports(unsigned ch,unsigned hz,unsigned fmt){return ch==2&&(hz==44100||hz==48000)&&fmt==SPS_FORMAT_S16_LE;}
/* SPS search callback uses errno semantics; supports() remains a boolean
 * predicate for configure(). Do not pass that predicate directly to search. */
static int check_configuration(unsigned ch,unsigned hz,unsigned fmt){return supports(ch,hz,fmt)?0:-EINVAL;}
static int32_t configuration(unsigned ch,unsigned hz,unsigned fmt){return search_for_suitable_configuration(ch,hz,fmt,check_configuration);}
static int configure(int32_t f,char **map){
 if(map)*map=NULL;
 unsigned hz=RATE_FROM_ENCODED_FORMAT(f);
 if(!supports(CHANNELS_FROM_ENCODED_FORMAT(f),hz,FORMAT_FROM_ENCODED_FORMAT(f)))return -EINVAL;
 audio_lock();
 /* configure is also called during SPS resynchronisation, even with an
  * unchanged output format. Preserve accepted PCM and the timestamp origin
  * of a healthy stream. Explicit flush/stop remain the discard boundaries. */
 aaudio_stream_state_t state=stream?p_AAudioStream_getState(stream):AAUDIO_STREAM_STATE_UNINITIALIZED;
 if(stream&&rate==hz&&!atomic_load(&stopping)&&
    (state==AAUDIO_STREAM_STATE_STARTING||state==AAUDIO_STREAM_STATE_STARTED)){
  audio_unlock();return 0;
 }
 close_locked();rate=hz;int r=open_locked();ya_diag_stream(rate,r);diag_poll_time=0;atomic_store(&stopping,r!=0);audio_unlock();return r;
}
static int play(void *buf,int frames,int type,uint32_t timestamp,uint64_t playtime){
 (void)type;(void)timestamp;(void)playtime;
 if(frames<0||(!buf&&frames))return -EINVAL;
 ya_diag_add(D_AUDIO_CALLS,1);ya_diag_add(D_REQUEST_FRAMES,(uint64_t)frames);
 int done=0,stalls=0;
 while(done<frames){
  pthread_testcancel();
  if(atomic_load(&stopping))return -ECANCELED;
  int volume_gate=ya_volume_gate();
  if(volume_gate<0){audio_lock();close_locked();atomic_store(&stopping,1);audio_unlock();return -EHOSTDOWN;}
  static const int16_t silence[512]={0};
  /* Recheck mute/fault between bounded writes, including audible PCM.
   * Samples already accepted by Android cannot be recalled here. */
  int amount=frames-done;if(amount>256)amount=256;
  const void *samples=volume_gate?(const void*)silence:(char*)buf+(size_t)done*4;
  uint64_t diag_before_lock=ya_diag_now();
  audio_lock();
  uint64_t diag_before_api=ya_diag_now();
  int n=stream?p_AAudioStream_write(stream,samples,amount,10000000):-ENODEV;
  uint64_t diag_after_api=ya_diag_now();
  ya_diag_write(amount,n,diag_before_lock,diag_before_api,diag_after_api);
  if(stream&&diag_after_api&&(!diag_poll_time||diag_after_api-diag_poll_time>=1000000000ULL)){
   if(p_AAudioStream_getXRunCount)ya_diag_set(G_XRUN,p_AAudioStream_getXRunCount(stream));
   if(p_AAudioStream_getBufferSizeInFrames)ya_diag_set(G_BUFFER_SIZE,p_AAudioStream_getBufferSizeInFrames(stream));
   if(p_AAudioStream_getBufferCapacityInFrames)ya_diag_set(G_CAPACITY,p_AAudioStream_getBufferCapacityInFrames(stream));
   if(p_AAudioStream_getFramesPerBurst)ya_diag_set(G_BURST,p_AAudioStream_getFramesPerBurst(stream));
   diag_poll_time=diag_after_api;
  }
  audio_unlock();
  if(n<0)return n;
  if(n==0){if(++stalls>=100)return -ETIMEDOUT;}else{done+=n;stalls=0;}
 }return 0;
}
static int delay(long *frames){
 if(!frames)return -EINVAL;
 audio_lock();int rc=-ENODEV;
 if(stream){
  int64_t pos=0,ns=0;
  ya_diag_add(D_TIMESTAMP_CALLS,1);
  rc=p_AAudioStream_getTimestamp(stream,CLOCK_MONOTONIC,&pos,&ns);
  if(rc)ya_diag_add(D_TIMESTAMP_ERRORS,1);
  if(!rc){
   struct timespec t={0};rc=clock_gettime(CLOCK_MONOTONIC,&t);
   int64_t now=(int64_t)t.tv_sec*1000000000+t.tv_nsec;
   if(!rc&&(now<ns||now-ns>2000000000LL))rc=-EAGAIN;
   if(!rc)pos+=(now-ns)*rate/1000000000LL;
  }else if(rc==AAUDIO_ERROR_INVALID_STATE){
   /* Startup timestamp unavailable: real framework counter, lower precision. */
   pos=p_AAudioStream_getFramesRead(stream);rc=pos<0?-EAGAIN:0;
  }
  if(!rc){int64_t pending=p_AAudioStream_getFramesWritten(stream)-pos;
   if(pending<0)ya_diag_add(D_NEGATIVE_DELAY,1);
   *frames=pending>0?pending:0;ya_diag_set(G_DELAY,*frames);}
 }
 audio_unlock();return rc;
}
/* Close/reopen discards queued samples without async pause/flush transition races. */
static void flush(void){audio_lock();close_locked();int stopped=atomic_load(&stopping);int r=stopped?0:open_locked();ya_diag_add(D_STREAM_FLUSHES,1);ya_diag_stream(rate,stopped?-1:r);diag_poll_time=0;if(r)atomic_store(&stopping,1);audio_unlock();}
static volume_range_t volume_range={.minimum_volume_dB=-3000,.maximum_volume_dB=0};
static output_parameters_t volume_parameters={.volume_range=&volume_range};
static output_parameters_t *parameters(void){return &volume_parameters;}
static void volume(double centidb){if(isfinite(centidb)&&centidb>=-3000&&centidb<=0)ya_volume_request((int)lround((centidb+3000)/30.0));}
static int mute(int on){if(on)ya_volume_request(0);return 0;}
audio_output audio_aaudio={.name="aaudio",.init=init,.deinit=deinit,.get_configuration=configuration,.configure=configure,.play=play,.stop=stop,.flush=flush,.delay=delay,.parameters=parameters,.volume=volume,.mute=mute};
/* Hardware-only logical volume: PCM fix remains unity. Only actual/requested mute sends zeros. */
