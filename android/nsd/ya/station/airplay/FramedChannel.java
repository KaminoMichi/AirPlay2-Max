package ya.station.airplay;
import java.io.*;
/* One reader/writer owns each socket. Idle is cancellable, not an expired lease.
 * Partial headers/payloads have one absolute deadline, never reset by EAGAIN. */
public final class FramedChannel {
 public interface Io {int read(byte[] b,int off,int len,int waitMs)throws IOException;int write(byte[] b,int off,int len,int waitMs)throws IOException;}
 public interface Clock {long now();}
 public interface Stop {boolean requested();}
 public static final Clock SYSTEM_CLOCK=new Clock(){public long now(){return System.nanoTime();}};
 public static final class Frame {public final byte[] bytes;public final long deadline;Frame(byte[] b,long d){bytes=b;deadline=d;}}
 private final Io io;private final Clock clock;private final Stop stop;
 private static final long FRAME_NS=1000000000L,TRANSACTION_NS=5500000000L;
 public FramedChannel(Io i,Clock c,Stop s){io=i;clock=c;stop=s;}
 private void check()throws IOException{if(stop.requested()||Thread.currentThread().isInterrupted())throw new InterruptedIOException("bridge stopping");}
 public long now(){return clock.now();}
 private int waitMs(long deadline)throws IOException{check();long left=deadline-clock.now();if(left<=0)throw new java.net.SocketTimeoutException("partial frame or ACK deadline");return (int)Math.min(250,(left+999999)/1000000);}
 public Frame read()throws IOException{
  byte[] h=new byte[4];int used=0;long frameEnd=0,transactionEnd=0;
  while(used<4){check();int n=io.read(h,used,4-used,used==0?250:waitMs(frameEnd));if(n<0){if(used==0)return null;throw new EOFException("partial header");}if(n==0)continue;
   if(used==0){long start=clock.now();frameEnd=start+FRAME_NS;transactionEnd=start+TRANSACTION_NS;}used+=n;
  }
  int size=((h[0]&255)<<24)|((h[1]&255)<<16)|((h[2]&255)<<8)|(h[3]&255);
  if(size<5||size>32768)throw new IOException("frame length");byte[] body=new byte[size];used=0;
  while(used<size){int n=io.read(body,used,size-used,waitMs(frameEnd));if(n<0)throw new EOFException("partial payload");used+=n;}
  return new Frame(body,transactionEnd);
 }
 public void ack(boolean ok,long deadline)throws IOException{
  byte[] b={0,0,0,(byte)(ok?0:1)};int used=0;long end=Math.min(deadline,clock.now()+500000000L);
  while(used<b.length){int n=io.write(b,used,b.length-used,waitMs(end));if(n<0)throw new EOFException("ACK peer closed");used+=n;}
 }
}
