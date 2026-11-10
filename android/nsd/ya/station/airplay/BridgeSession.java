package ya.station.airplay;
import java.io.*;
public final class BridgeSession {
 public interface Factory {NsdPublisher create();}
 private final FramedChannel channel;private final Factory factory;
 public BridgeSession(FramedChannel c,Factory f){channel=c;factory=f;}
 public void run()throws IOException,InterruptedException{
  NsdPublisher publisher=null;
  try{for(;;){
   FramedChannel.Frame f=channel.read();if(f==null)return;BridgeProtocol p=new BridgeProtocol(f.bytes);
   long callbackEnd=f.deadline-500000000L;boolean ok=true;
   if(p.op=='Q'){if(publisher!=null){ok=publisher.closeAwait(callbackEnd);if(ok)publisher=null;}channel.ack(ok,f.deadline);return;}
   if(p.op=='R'&&publisher!=null)throw new IOException("already registered");
   if(p.op=='U'&&publisher==null)throw new IOException("update without registration");
   if(publisher!=null){ok=publisher.closeAwait(callbackEnd);if(ok)publisher=null;}
   if(ok){publisher=factory.create();ok=publisher.publish(p,callbackEnd);}
   channel.ack(ok,f.deadline);if(!ok)return;
  }}finally{
   if(publisher!=null&&!publisher.closeAwait())throw new IOException("NSD unregister not acknowledged");
  }
 }
}
