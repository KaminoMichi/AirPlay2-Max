package ya.station.airplay;
import android.app.*;
import android.content.*;
import android.net.*;
import android.os.IBinder;
import android.util.Log;
import java.io.*;
public final class AirplayNsdService extends Service {
 private volatile boolean closing;private LocalServerSocket server;private LocalSocket active;private Thread worker;
 @Override public synchronized int onStartCommand(Intent intent,int flags,int startId){
  if(worker!=null)return START_NOT_STICKY;
  try{
   Notification.Builder b=new Notification.Builder(this).setSmallIcon(android.R.drawable.ic_media_play).setContentTitle("YA AirPlay").setContentText("Local Bonjour bridge RC1.2");
   if(android.os.Build.VERSION.SDK_INT>=26){
    Class<?> c=Class.forName("android.app.NotificationChannel");Object ch=c.getConstructor(String.class,CharSequence.class,int.class).newInstance("ya_airplay","YA AirPlay",2);
    Object nm=getSystemService(NOTIFICATION_SERVICE);nm.getClass().getMethod("createNotificationChannel",c).invoke(nm,ch);
    Notification.Builder.class.getMethod("setChannelId",String.class).invoke(b,"ya_airplay");
   }
   startForeground(1,b.build());server=new LocalServerSocket("ya-airplay-nsd-v1");
  }catch(Exception e){Log.e("YA_AIRPLAY_NSD","startup failed",e);stopSelf();return START_NOT_STICKY;}
  worker=new Thread(new Runnable(){public void run(){serve();}},"ya-nsd-bridge");worker.start();return START_NOT_STICKY;
 }
 private void serve(){
  while(!closing){
   LocalSocket client=null;
   try{
    client=server.accept();synchronized(this){if(closing){client.close();break;}active=client;}
    int uid=client.getPeerCredentials().getUid();if(uid!=0&&uid!=2000)throw new IOException("bridge peer denied");
    // Timeout classification is deliberately owned by Os.poll + numeric errno.
    client.setSoTimeout(0);
    FramedChannel channel=new FramedChannel(new AndroidSocketIo(client.getFileDescriptor()),FramedChannel.SYSTEM_CLOCK,new FramedChannel.Stop(){public boolean requested(){return closing;}});
    new BridgeSession(channel,new BridgeSession.Factory(){public NsdPublisher create(){return new NsdPublisher(AirplayNsdService.this);}}).run();
   }catch(EOFException expected){/* partial EOF: BridgeSession still unregisters */}
   catch(Exception e){if(!closing)Log.e("YA_AIRPLAY_NSD","bridge session failed",e);}
   finally{
    if(client!=null)try{client.close();}catch(IOException ignored){}
    synchronized(this){active=null;}
   }
  }
 }
 @Override public synchronized void onDestroy(){
  closing=true;
  // Wake the single reader, which owns final close; do not block main looper
  // waiting for NSD callbacks delivered on that same looper.
  if(active!=null)try{active.shutdownInput();}catch(IOException ignored){}
  if(server!=null)try{server.close();}catch(IOException ignored){}
  stopForeground(true);super.onDestroy();
 }
 @Override public IBinder onBind(Intent i){return null;}
}
