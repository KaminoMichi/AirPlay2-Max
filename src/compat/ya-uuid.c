/* Android subset of libuuid used by pinned SPS; fail closed on entropy failure. */
#include "uuid/uuid.h"
#include <openssl/sha.h>
#include <sys/random.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static int hex(char c) {
 if(c>='0'&&c<='9') return c-'0';
 if(c>='a'&&c<='f') return c-'a'+10;
 if(c>='A'&&c<='F') return c-'A'+10;
 return -1;
}
int uuid_parse(const char *s, uuid_t out) {
 uuid_t tmp; int p=0;
 if(!s || strlen(s)!=36) return -1;
 for(int i=0;i<16;i++) {
  if(p==8||p==13||p==18||p==23) {if(s[p++]!='-') return -1;}
  int a=hex(s[p++]), b=hex(s[p++]); if(a<0||b<0) return -1;
  tmp[i]=(a<<4)|b;
 }
 memcpy(out,tmp,16); return 0;
}
void uuid_unparse_lower(const uuid_t in, char *s) {
 static const char h[]="0123456789abcdef"; int p=0;
 for(int i=0;i<16;i++) {
  if(i==4||i==6||i==8||i==10) s[p++]='-';
  s[p++]=h[in[i]>>4]; s[p++]=h[in[i]&15];
 } s[p]=0;
}
void uuid_generate_random(uuid_t out) {
 size_t off=0;
 while(off<16) {
  ssize_t n=getrandom(out+off,16-off,0);
  if(n<0&&errno==EINTR) continue;
  if(n<=0) abort();
  off+=(size_t)n;
 }
 out[6]=(out[6]&15)|0x40; out[8]=(out[8]&63)|0x80;
}
void uuid_generate_sha1(uuid_t out,const uuid_t ns,const char *name,size_t len) {
 SHA_CTX ctx; unsigned char digest[SHA_DIGEST_LENGTH];
 if(!SHA1_Init(&ctx)||!SHA1_Update(&ctx,ns,16)||!SHA1_Update(&ctx,name,len)||!SHA1_Final(digest,&ctx)) abort();
 memcpy(out,digest,16); out[6]=(out[6]&15)|0x50; out[8]=(out[8]&63)|0x80;
}
