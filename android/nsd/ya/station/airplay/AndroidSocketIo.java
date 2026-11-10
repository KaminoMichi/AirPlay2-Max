package ya.station.airplay;
import android.system.*;
import java.io.*;
import java.net.*;
/* Public API21+, compiled against SDK23. No LocalSocket InputStream timeout or
 * localized IOException-message matching. Protocol never carries SCM_RIGHTS. */
public final class AndroidSocketIo implements FramedChannel.Io {
 // Linux/Bionic arm64 socket ABI, verified in NDK sys/socket.h. These are
 // not public OsConstants fields in SDK23. EWOULDBLOCK equals EAGAIN here.
 private static final int MSG_DONTWAIT=0x40,MSG_NOSIGNAL=0x4000;
 private final FileDescriptor fd;
 public AndroidSocketIo(FileDescriptor f){fd=f;}
 private boolean ready(short events,int timeout)throws IOException{
  if(!fd.valid())throw new IOException("bridge socket closed");
  StructPollfd p=new StructPollfd();p.fd=fd;p.events=events;
  try{int n=Os.poll(new StructPollfd[]{p},timeout);if((p.revents&OsConstants.POLLNVAL)!=0)throw new IOException("bridge invalid descriptor");return n>0;}
  catch(ErrnoException e){if(e.errno==OsConstants.EINTR||e.errno==OsConstants.EAGAIN){retry(e);return false;}throw new IOException("bridge poll errno="+e.errno,e);}
 }
 private int retry(ErrnoException e)throws IOException{
  if(e.errno!=OsConstants.EAGAIN&&e.errno!=OsConstants.EINTR)throw new IOException("bridge socket errno="+e.errno,e);
  // A readiness race or repeated spurious readiness must not busy-spin.
  try{Thread.sleep(5);}catch(InterruptedException x){Thread.currentThread().interrupt();throw new InterruptedIOException("bridge interrupted");}return 0;
 }
 public int read(byte[] b,int off,int len,int waitMs)throws IOException{
  if(!ready((short)(OsConstants.POLLIN|OsConstants.POLLHUP|OsConstants.POLLERR),waitMs))return 0;
  try{int n=Os.recvfrom(fd,b,off,len,MSG_DONTWAIT,(InetSocketAddress)null);return n==0?-1:n;}
  catch(ErrnoException e){return retry(e);}
 }
 public int write(byte[] b,int off,int len,int waitMs)throws IOException{
  if(!ready((short)(OsConstants.POLLOUT|OsConstants.POLLHUP|OsConstants.POLLERR),waitMs))return 0;
  try{return Os.sendto(fd,b,off,len,MSG_DONTWAIT|MSG_NOSIGNAL,(InetAddress)null,0);}
  catch(ErrnoException e){return retry(e);}
 }
}
