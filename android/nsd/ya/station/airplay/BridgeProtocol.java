package ya.station.airplay;
import java.io.*;
import java.nio.charset.*;
public final class BridgeProtocol {
 public final char op;public final int port;public final String raopName,airplayName;public final String[] raopTxt,airplayTxt;
 private static String text(DataInputStream d)throws IOException{
  int n=d.readUnsignedShort();if(n>255)throw new IOException("field length");byte[] b=new byte[n];d.readFully(b);
  try{return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).decode(java.nio.ByteBuffer.wrap(b)).toString();}
  catch(CharacterCodingException e){throw new IOException("UTF-8",e);}
 }
 private static String[] records(DataInputStream d)throws IOException{
  int n=d.readUnsignedShort();if(n>128)throw new IOException("TXT count");String[] a=new String[n];java.util.HashSet<String> keys=new java.util.HashSet<>();
  for(int i=0;i<n;i++){a[i]=text(d);int eq=a[i].indexOf('=');if(eq<1)throw new IOException("TXT key");String k=a[i].substring(0,eq);for(int j=0;j<k.length();j++)if(k.charAt(j)<32||k.charAt(j)>126)throw new IOException("ASCII key");if(!keys.add(k.toLowerCase(java.util.Locale.ROOT)))throw new IOException("duplicate key");}return a;
 }
 public BridgeProtocol(byte[] packet)throws IOException{
  DataInputStream d=new DataInputStream(new ByteArrayInputStream(packet));if(d.readInt()!=0x594e5331)throw new IOException("magic");op=(char)d.readUnsignedByte();
  if(op=='Q'){port=0;raopName=airplayName=null;raopTxt=airplayTxt=null;}
  else if(op=='R'||op=='U'){port=d.readUnsignedShort();raopName=text(d);airplayName=text(d);raopTxt=records(d);airplayTxt=records(d);if(port==0||raopName.isEmpty()||airplayName.isEmpty())throw new IOException("service identity");}
  else throw new IOException("operation");if(d.available()!=0)throw new IOException("trailing data");
 }
 public static BridgeProtocol read(DataInputStream d)throws IOException{return read(d,d.readUnsignedByte());}
 public static BridgeProtocol read(DataInputStream d,int first)throws IOException{int n=(first<<24)|(d.readUnsignedByte()<<16)|(d.readUnsignedByte()<<8)|d.readUnsignedByte();if(n<5||n>32768)throw new IOException("frame length");byte[] b=new byte[n];d.readFully(b);return new BridgeProtocol(b);}
}
