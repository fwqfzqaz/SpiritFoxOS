/* Exercise the guest update state machine and transaction engine together. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"
#include "txn.h"
#include "package.h"
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"UPDATE FAIL line %d\n",__LINE__);exit(1);}}while(0)
static uint8_t stable[4096*512],cache[4096*512],baseline[4096*512],pristine[4096*512],dirty[4096];
static SfTxn volume,recovered;
static int operations,fail_at,mode;
static int read_sector(void*c,uint64_t lba,uint8_t out[512]){if(lba>=4096)return SF_INVALID;memcpy(out,cache+lba*512,512);return 0;}
static void subset(void){for(unsigned i=0;i<4096;i++)if(dirty[i]&&((i*17+fail_at)%3)==0)memcpy(stable+i*512,cache+i*512,512);}
static int write_sector(void*c,uint64_t lba,const uint8_t in[512]){
 CHECK(lba>=2049&&lba<2563);if(++operations==fail_at){if(mode){subset();memcpy(stable+lba*512,in,mode==1?257:512);}return SF_DEVICE_GONE;}
 memcpy(cache+lba*512,in,512);dirty[lba]=1;return 0;
}
static int flush(void*c){
 if(++operations==fail_at){if(mode==1)subset();if(mode==2)memcpy(stable,cache,sizeof(stable));return SF_DEVICE_GONE;}
 memcpy(stable,cache,sizeof(stable));memset(dirty,0,sizeof(dirty));return 0;
}
static void boot(SfTxn*t){memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));CHECK(!sf_txn_mount(t,read_sector,write_sector,flush,0));}
static int store(SfTxn*t,const SfUpdate*s,const char*package){
 uint8_t encoded[SF_UPDATE_BYTES];CHECK(!sf_update_encode(s,encoded));
 const char*paths[]={"/boot/current",s->candidate=='b'?"/slots/b.sfk":"/slots/a.sfk"};const void*data[]={encoded,package};uint32_t lengths[]={sizeof(encoded),package?(uint32_t)strlen(package):0};
 return sf_txn_commit(t,paths,data,lengths,package?2:1);
}
static SfUpdate load(SfTxn*t){uint8_t bytes[SF_UPDATE_BYTES];SfUpdate s;CHECK(sf_txn_get(t,"/boot/current",bytes,sizeof(bytes))==sizeof(bytes));CHECK(!sf_update_decode(&s,bytes,sizeof(bytes)));return s;}
static void consistent(SfTxn*t,const SfUpdate*s){
 char data[16];uint8_t hash[32];uint8_t slots[]={s->good,s->candidate};
 for(unsigned i=0;i<2;i++)if(slots[i]){long n=sf_txn_get(t,slots[i]=='a'?"/slots/a.sfk":"/slots/b.sfk",data,sizeof(data));CHECK(n>0);sf_sha256(hash,(uint8_t*)data,n);CHECK(!memcmp(hash,i?s->candidate_hash:s->good_hash,32));}
}
int main(void){
 uint8_t hash_a[32],hash_b[32],bytes[SF_UPDATE_BYTES],copy[SF_UPDATE_BYTES];sf_sha256(hash_a,(uint8_t*)"healthy-a",9);sf_sha256(hash_b,(uint8_t*)"trial-b",7);
 SfUpdate empty={0},s=empty,out;CHECK(!sf_update_encode(&s,bytes));CHECK(!sf_update_decode(&out,bytes,sizeof(bytes)));
 CHECK(sf_update_begin(&s)==SF_INVALID&&sf_update_finish(&s,1)==SF_INVALID);
 CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_a)&&s.candidate=='a'&&!s.good);
 CHECK(sf_update_stage(&s,"org.spiritfox.demo",hash_b)==SF_BUSY);CHECK(!sf_update_begin(&s));CHECK(sf_update_begin(&s)==SF_INVALID);CHECK(!sf_update_finish(&s,0));CHECK(!memcmp(&s,&empty,sizeof(s)));
 CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_a));CHECK(!sf_update_begin(&s));CHECK(!sf_update_finish(&s,1));CHECK(s.good=='a'&&!s.candidate);
 SfUpdate good=s;CHECK(sf_update_stage(&s,"org.other",hash_b)==SF_DEPENDENCY&&!memcmp(&s,&good,sizeof(s)));
 CHECK(sf_update_stage(&s,"bad/name",hash_b)==SF_BAD_FORMAT);CHECK(sf_update_stage(&s,"",hash_b)==SF_BAD_FORMAT);
 char huge[65];memset(huge,'a',64);huge[64]=0;CHECK(sf_update_stage(&s,huge,hash_b)==SF_BAD_FORMAT);
 uint8_t zero[32]={0};CHECK(sf_update_stage(&s,"org.spiritfox.demo",zero)==SF_BAD_FORMAT);
 CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_b)&&s.candidate=='b');CHECK(!sf_update_begin(&s));CHECK(!sf_update_finish(&s,0));CHECK(!memcmp(&s,&good,sizeof(s)));
 CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_b));CHECK(!sf_update_encode(&s,bytes));
 for(unsigned n=0;n<SF_UPDATE_BYTES;n++)CHECK(sf_update_decode(&out,bytes,n)==SF_BAD_FORMAT);
 /* Accepted byte mutations must still have a unique canonical encoding. */
 for(unsigned i=0;i<sizeof(bytes);i++)for(unsigned bit=0;bit<8;bit++){
  memcpy(copy,bytes,sizeof(copy));copy[i]^=1u<<bit;int r=sf_update_decode(&out,copy,sizeof(copy));CHECK(r==SF_BAD_FORMAT||r==0);
  if(!r){uint8_t encoded[SF_UPDATE_BYTES];CHECK(!sf_update_encode(&out,encoded)&&!memcmp(encoded,copy,sizeof(copy)));}
 }
 memcpy(copy,bytes,sizeof(copy));copy[8]=3;CHECK(sf_update_decode(&out,copy,sizeof(copy))==SF_BAD_FORMAT);
 memcpy(copy,bytes,sizeof(copy));copy[10]=copy[9];CHECK(sf_update_decode(&out,copy,sizeof(copy))==SF_BAD_FORMAT);
 memcpy(copy,bytes,sizeof(copy));memset(copy+96,'a',64);CHECK(sf_update_decode(&out,copy,sizeof(copy))==SF_BAD_FORMAT);
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(pristine,1,sizeof(pristine),f)==sizeof(pristine));fclose(f);
 for(unsigned phase=0;phase<4;phase++){
  memcpy(stable,pristine,sizeof(stable));fail_at=0;boot(&volume);s=empty;
  CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_a));CHECK(!store(&volume,&s,"healthy-a"));CHECK(!sf_update_begin(&s));CHECK(!store(&volume,&s,0));CHECK(!sf_update_finish(&s,1));CHECK(!store(&volume,&s,0));
  if(phase){CHECK(!sf_update_stage(&s,"org.spiritfox.demo",hash_b));CHECK(!store(&volume,&s,"trial-b"));}
  if(phase>=2){CHECK(!sf_update_begin(&s));CHECK(!store(&volume,&s,0));}
  memcpy(baseline,stable,sizeof(stable));SfUpdate next=s;
  if(!phase)CHECK(!sf_update_stage(&next,"org.spiritfox.demo",hash_b));else if(phase==1)CHECK(!sf_update_begin(&next));else CHECK(!sf_update_finish(&next,phase==2));
  operations=0;CHECK(!store(&volume,&next,!phase?"trial-b":0));CHECK(operations==261);
  for(mode=0;mode<3;mode++)for(int fault=1;fault<=261;fault++){
   memcpy(stable,baseline,sizeof(stable));fail_at=0;boot(&volume);operations=0;fail_at=fault;
   CHECK(store(&volume,&next,!phase?"trial-b":0)==SF_COMMIT_UNCERTAIN&&!volume.ready);fail_at=0;boot(&recovered);out=load(&recovered);consistent(&recovered,&out);
   CHECK(out.good=='a'||(phase==2&&out.good=='b'&&out.phase==SF_UPDATE_STABLE));
   if(out.phase==SF_UPDATE_TRYING){CHECK(!sf_update_finish(&out,0));CHECK(!store(&recovered,&out,0));boot(&recovered);out=load(&recovered);CHECK(out.good=='a'&&out.phase==SF_UPDATE_STABLE);}
  }
 }
 printf("UPDATE PASS: %u checks, stage/begin/confirm/rollback x 261 boundaries x 3 fault modes; canonical state and interrupted-trial recovery\n",checks);return 0;
}
