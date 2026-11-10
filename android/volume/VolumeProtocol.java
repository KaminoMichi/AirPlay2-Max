package ya.station.airplay.volume;
import java.io.*;
import java.nio.charset.StandardCharsets;
/* One bounded request in flight. Parser is shared with host tests. */
public final class VolumeProtocol {
 public interface Audio {int get()throws Exception;int min()throws Exception;int max()throws Exception;void set(int n)throws Exception;}
 public interface Io {int read(byte[]b,int off,int n,int ms)throws IOException;int write(byte[]b,int off,int n,int ms)throws IOException;}
 public static boolean authorized(int uid,int pid,int own,int expected){return own==0&&uid==own&&pid==expected&&expected>1;}
 public static String command(String line,Audio audio)throws Exception{
  if(!line.matches("[GS] [1-9][0-9]{0,8}( [0-9]{1,3})?"))throw new IOException("syntax");
  String[]a=line.split(" ");int seq=Integer.parseInt(a[1]);
  if((a[0].equals("G")&&a.length!=2)||(a[0].equals("S")&&a.length!=3))throw new IOException("arity");
  int min=audio.min(),max=audio.max();if(min!=0||max!=100){System.err.println("YA_VOLUME unsupported_range min="+min+" max="+max);throw new IOException("unsupported range");}
  if(a[0].equals("S")){int idx=Integer.parseInt(a[2]);if(idx>100)throw new IOException("range");audio.set(idx);}
  int n=audio.get();if(n<0||n>100)throw new IOException("invalid AudioService value");
  return "V 1 "+seq+" "+n+" "+min+" "+max+"\n";
 }
 public static String readLine(Io io)throws IOException{
  byte[]b=new byte[48];int used=0;long end=System.nanoTime()+700000000L;
  while(used<b.length){if(System.nanoTime()>=end)throw new java.net.SocketTimeoutException("request deadline");
   int n=io.read(b,used,1,50);if(n<0){if(used==0)return null;throw new EOFException("partial line");}
   if(n==0)continue;if(b[used]=='\n')return new String(b,0,used,StandardCharsets.US_ASCII);used++;
  }throw new IOException("line too long");
 }
 public static void write(Io io,String s)throws IOException{
  byte[]b=s.getBytes(StandardCharsets.US_ASCII);int used=0;long end=System.nanoTime()+300000000L;
  while(used<b.length){if(System.nanoTime()>=end)throw new java.net.SocketTimeoutException("response deadline");int n=io.write(b,used,b.length-used,50);if(n<0)throw new EOFException("response EOF");used+=n;}
 }
 private VolumeProtocol(){}
}
