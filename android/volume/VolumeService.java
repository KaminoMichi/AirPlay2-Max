package ya.station.airplay.volume;
import android.net.*;
import java.io.*;
/* Direct child of receiver; no APK, Android Service, receiver or global hooks. */
public final class VolumeService {
 private static volatile String phase="bootstrap";
 private static volatile long deadline=System.nanoTime()+4000000000L;
 public static void main(String[]args){
  Thread watchdog=new Thread(new Runnable(){public void run(){for(;;){if(System.nanoTime()>deadline){System.err.println("YA_VOLUME watchdog phase="+phase);Runtime.getRuntime().halt(74);}try{Thread.sleep(20);}catch(InterruptedException e){return;}}}},"volume-watchdog");watchdog.setDaemon(true);watchdog.start();
  int result=0;
  try{
   if(args.length!=2||!args[0].matches("ya-airplay-volume-v14-[0-9]+"))throw new IOException("arguments");
   int parent=Integer.parseInt(args[1]);
   if(android.os.Process.myUid()!=0)throw new SecurityException("root required");
   LocalServerSocket server=new LocalServerSocket(args[0]);
   try{
    boolean served=false;
    while(!served){
     try(LocalSocket socket=server.accept()){
      Credentials peer=socket.getPeerCredentials();
      if(!VolumeProtocol.authorized(peer.getUid(),peer.getPid(),android.os.Process.myUid(),parent))continue;
      served=true;VolumeProtocol.Io io=new AndroidSocketIo(socket.getFileDescriptor());
      phase="binder_init";BinderAudio audio=new BinderAudio();
      for(;;){phase="read";deadline=System.nanoTime()+1500000000L;String line=VolumeProtocol.readLine(io);if(line==null)break;
       phase="audio";deadline=System.nanoTime()+800000000L;String response=VolumeProtocol.command(line,audio);phase="write";VolumeProtocol.write(io,response);
      }
     }
    }
   }finally{server.close();}
  }catch(Exception e){Throwable cause=e;for(int i=0;i<8&&cause.getCause()!=null;i++)cause=cause.getCause();
   int errno=cause instanceof android.system.ErrnoException?((android.system.ErrnoException)cause).errno:0;
   System.err.println("YA_VOLUME bridge_failed phase="+phase+" class="+cause.getClass().getSimpleName()+" errno="+errno);result=70;}
  // Binder thread pool must not leave a root orphan after main returns.
  Runtime.getRuntime().halt(result);
 }
}
