package ya.station.airplay;
import android.content.Context;
import android.util.Log;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
/* All waits are on the socket worker; NSD callbacks may need the main looper. */
public final class NsdPublisher {
 private final NsdManager manager;private volatile Entry[] entries;private volatile boolean closing;
 private static final int PENDING=0,REGISTERED=1,REMOVING=2,GONE=3,REMOVE_FAILED=4;
 private final class Entry implements NsdManager.RegistrationListener {
  final String name,type;final int port;final CountDownLatch ready=new CountDownLatch(1);
  CountDownLatch gone=new CountDownLatch(1);volatile int state=PENDING;volatile String actual;
  Entry(String n,String t,int p){name=n;type=t;port=p;}
  public synchronized void onServiceRegistered(NsdServiceInfo s){state=REGISTERED;actual=s.getServiceName();Log.i("YA_AIRPLAY_NSD","registered type="+type+" port="+port);ready.countDown();if(closing)unregister();}
  public synchronized void onRegistrationFailed(NsdServiceInfo s,int error){state=GONE;Log.e("YA_AIRPLAY_NSD","registration failed type="+type+" error="+error);ready.countDown();gone.countDown();}
  public synchronized void onServiceUnregistered(NsdServiceInfo s){state=GONE;Log.i("YA_AIRPLAY_NSD","unregistered type="+type);gone.countDown();}
  public synchronized void onUnregistrationFailed(NsdServiceInfo s,int error){state=REMOVE_FAILED;Log.e("YA_AIRPLAY_NSD","unregister failed type="+type+" error="+error);gone.countDown();}
  synchronized CountDownLatch unregister(){
   if(state==REGISTERED||state==REMOVE_FAILED){
    if(state==REMOVE_FAILED)gone=new CountDownLatch(1);
    state=REMOVING;
    try{manager.unregisterService(this);}catch(RuntimeException e){state=REMOVE_FAILED;gone.countDown();}
   }
   return gone;
  }
 }
 public NsdPublisher(Context context){manager=(NsdManager)context.getSystemService(Context.NSD_SERVICE);if(manager==null)throw new IllegalStateException("NSD missing");}
 private static NsdServiceInfo info(String name,String type,int port,String[] txt){
  NsdServiceInfo s=new NsdServiceInfo();s.setServiceName(name);s.setServiceType(type);s.setPort(port);
  for(String record:txt){int eq=record.indexOf('=');s.setAttribute(record.substring(0,eq),record.substring(eq+1));}return s;
 }
 public boolean publish(BridgeProtocol p)throws InterruptedException{return publish(p,Long.MAX_VALUE);}
 public boolean publish(BridgeProtocol p,long transactionEnd)throws InterruptedException{
  if(closing||entries!=null)throw new IllegalStateException("publisher closed or already used");
  entries=new Entry[]{new Entry(p.raopName,"_raop._tcp",p.port),new Entry(p.airplayName,"_airplay._tcp",p.port)};
  NsdServiceInfo[] infos={info(p.raopName,"_raop._tcp.",p.port,p.raopTxt),info(p.airplayName,"_airplay._tcp.",p.port,p.airplayTxt)};
  for(int i=0;i<2;i++){try{manager.registerService(infos[i],NsdManager.PROTOCOL_DNS_SD,entries[i]);}catch(RuntimeException e){entries[i].onRegistrationFailed(infos[i],-1);}}
  long deadline=Math.min(transactionEnd,System.nanoTime()+TimeUnit.SECONDS.toNanos(3));
  for(Entry e:entries){long left=deadline-System.nanoTime();if(left<=0||!e.ready.await(left,TimeUnit.NANOSECONDS)||e.state!=REGISTERED||!e.name.equals(e.actual)){closeAwait(transactionEnd);return false;}}
  return !closing;
 }
 public boolean closeAwait()throws InterruptedException{return closeAwait(Long.MAX_VALUE);}
 public boolean closeAwait(long transactionEnd)throws InterruptedException{
  closing=true;Entry[] snapshot=entries;if(snapshot==null)return true;boolean ok=true;
  long deadline=Math.min(transactionEnd,System.nanoTime()+TimeUnit.SECONDS.toNanos(2));
  // Issue BOTH removals before waiting; preserve each attempt's latch across retry.
  CountDownLatch[] attempts=new CountDownLatch[snapshot.length];
  for(int i=0;i<snapshot.length;i++)attempts[i]=snapshot[i].unregister();
  for(int i=0;i<snapshot.length;i++){long left=deadline-System.nanoTime();if(left<=0||!attempts[i].await(Math.max(1,left),TimeUnit.NANOSECONDS)||snapshot[i].state!=GONE)ok=false;}
  return ok;
 }
}
