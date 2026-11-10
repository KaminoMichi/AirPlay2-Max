package ya.station.airplay.volume;
import java.lang.reflect.*;
/* Hidden Binder interface, verified by capability checks, never transaction IDs. */
public final class BinderAudio implements VolumeProtocol.Audio {
 private final Object service;private final Method get,min,max,set;private final Object binder;private final Method alive;
 public BinderAudio()throws Exception{
  Class<?>ib=Class.forName("android.os.IBinder");
  binder=Class.forName("android.os.ServiceManager").getMethod("getService",String.class).invoke(null,"audio");
  if(binder==null)throw new IllegalStateException("audio unavailable");alive=ib.getMethod("isBinderAlive");
  service=Class.forName("android.media.IAudioService$Stub").getMethod("asInterface",ib).invoke(null,binder);
  Class<?>c=Class.forName("android.media.IAudioService");get=c.getMethod("getStreamVolume",int.class);min=c.getMethod("getStreamMinVolume",int.class);max=c.getMethod("getStreamMaxVolume",int.class);set=c.getMethod("setStreamVolume",int.class,int.class,int.class,String.class);
 }
 private void check()throws Exception{if(!(Boolean)alive.invoke(binder))throw new IllegalStateException("audio died");}
 public int get()throws Exception{check();return (Integer)get.invoke(service,3);}
 public int min()throws Exception{check();return (Integer)min.invoke(service,3);}
 public int max()throws Exception{check();return (Integer)max.invoke(service,3);}
 public void set(int n)throws Exception{check();set.invoke(service,3,n,0,"com.android.shell");}
}
