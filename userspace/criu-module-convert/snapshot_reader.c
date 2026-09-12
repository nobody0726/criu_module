#include "snapshot_reader.h"
#include "../../include/criu_snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Small SHA-256 implementation; the snapshot ABI stores its first 64 bits. */
struct sha256 { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t used; };
static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static const uint32_t K[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static void sha_block(struct sha256 *s, const uint8_t *p) {
	uint32_t w[64],a,b,c,d,e,f,g,h,t1,t2; unsigned i;
	for (i=0;i<16;i++) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
	for (;i<64;i++) { uint32_t x=w[i-15], y=w[i-2]; w[i]=(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-16]+(rotr(y,17)^rotr(y,19)^(y>>10))+w[i-7]; }
	a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
	for(i=0;i<64;i++){t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^((~e)&g))+K[i]+w[i];t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
	s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(struct sha256 *s){static const uint32_t h[]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->h,h,sizeof(h));s->n=0;s->used=0;}
static void sha_update(struct sha256 *s,const uint8_t *p,size_t n){s->n+=n;while(n){size_t k=64-s->used;if(k>n)k=n;memcpy(s->b+s->used,p,k);s->used+=k;p+=k;n-=k;if(s->used==64){sha_block(s,s->b);s->used=0;}}}
static void sha_final(struct sha256 *s,uint8_t out[32]){uint64_t bits=s->n*8;size_t i,pad=s->used<56?56-s->used:120-s->used;uint8_t z[128]={0};z[0]=0x80;sha_update(s,z,pad);memset(z,0,128);for(i=0;i<8;i++)z[7-i]=(uint8_t)(bits>>(8*i));sha_update(s,z,8);for(i=0;i<8;i++){out[4*i]=s->h[i]>>24;out[4*i+1]=s->h[i]>>16;out[4*i+2]=s->h[i]>>8;out[4*i+3]=s->h[i];}}

static uint16_t le16(const uint8_t *p){return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t le64(const uint8_t *p){return (uint64_t)le32(p)|(uint64_t)le32(p+4)<<32;}
static int known_type(uint16_t t){return t>=CRIU_SNAPSHOT_REC_TASK && t<=CRIU_SNAPSHOT_REC_PAGE_RUN;}

int snapshot_read_validate(const char *path, struct snapshot_document *doc)
{
	int fd; struct stat st; size_t got=0, off, body_end; uint32_t count=0; int saw_end=0; uint8_t digest[32]; struct sha256 sha; uint64_t expected;
	if (!path || !doc) return SNAPSHOT_READER_FORMAT_ERROR; memset(doc,0,sizeof(*doc));
	fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0)return SNAPSHOT_READER_IO_ERROR;
	if(fstat(fd,&st)<0){close(fd);return SNAPSHOT_READER_IO_ERROR;}
	if(st.st_size<0 || (uint64_t)st.st_size>CRIU_SNAPSHOT_MAX_TOTAL_SIZE){close(fd);return SNAPSHOT_READER_FORMAT_ERROR;}
	doc->size=(size_t)st.st_size; if(doc->size<CRIU_SNAPSHOT_HEADER_SIZE+CRIU_SNAPSHOT_FOOTER_SIZE){close(fd);return SNAPSHOT_READER_FORMAT_ERROR;}
	doc->data=malloc(doc->size); if(!doc->data){close(fd);return SNAPSHOT_READER_IO_ERROR;}
	while(got<doc->size){ssize_t n=read(fd,doc->data+got,doc->size-got);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);snapshot_document_free(doc);return SNAPSHOT_READER_IO_ERROR;}got+=(size_t)n;} close(fd);
	if(le64(doc->data)!=CRIU_SNAPSHOT_MAGIC||le32(doc->data+8)!=CRIU_SNAPSHOT_VERSION||le16(doc->data+12)!=CRIU_SNAPSHOT_HEADER_SIZE||le16(doc->data+14)!=CRIU_SNAPSHOT_HEADER_FLAGS||le32(doc->data+44)!=0)return SNAPSHOT_READER_FORMAT_ERROR;
	if(le64(doc->data+48)!=doc->size||le64(doc->data+48)>CRIU_SNAPSHOT_MAX_TOTAL_SIZE)return SNAPSHOT_READER_FORMAT_ERROR;
	expected=le64(doc->data+56); memset(doc->data+56,0,8); sha_init(&sha); sha_update(&sha,doc->data,doc->size-CRIU_SNAPSHOT_FOOTER_SIZE); sha_final(&sha,digest); memcpy(doc->data+56,&expected,8);
	if(le64(digest)!=expected)return SNAPSHOT_READER_FORMAT_ERROR;
	body_end=doc->size-CRIU_SNAPSHOT_FOOTER_SIZE; off=CRIU_SNAPSHOT_HEADER_SIZE;
	while(off<body_end){uint16_t type;uint16_t flags;uint32_t reserved;uint64_t len;if(body_end-off<CRIU_SNAPSHOT_TLV_HEADER_SIZE)return SNAPSHOT_READER_FORMAT_ERROR;type=le16(doc->data+off);flags=le16(doc->data+off+2);reserved=le32(doc->data+off+4);len=le64(doc->data+off+8);if(flags||reserved||len>CRIU_SNAPSHOT_MAX_RECORD_SIZE||len>body_end-off-CRIU_SNAPSHOT_TLV_HEADER_SIZE)return SNAPSHOT_READER_FORMAT_ERROR;off+=CRIU_SNAPSHOT_TLV_HEADER_SIZE;if(++count>CRIU_SNAPSHOT_MAX_RECORDS)return SNAPSHOT_READER_FORMAT_ERROR;if(type==CRIU_SNAPSHOT_REC_END){if(len||off+len!=body_end||saw_end)return SNAPSHOT_READER_FORMAT_ERROR;saw_end=1;}else if(!known_type(type)){if(type<0x8000)return SNAPSHOT_READER_UNSUPPORTED;}off+=(size_t)len;}
	if(!saw_end||off!=body_end||le64(doc->data+doc->size-24)!=CRIU_SNAPSHOT_MAGIC||le32(doc->data+doc->size-16)!=CRIU_SNAPSHOT_VERSION||le32(doc->data+doc->size-12)!=count||le64(doc->data+doc->size-8)!=expected||le32(doc->data+40)!=count)return SNAPSHOT_READER_FORMAT_ERROR;
	doc->record_count=count; return SNAPSHOT_READER_OK;
}
void snapshot_document_free(struct snapshot_document *doc){if(doc){free(doc->data);doc->data=NULL;doc->size=0;doc->record_count=0;}}
