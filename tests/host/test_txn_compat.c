/* Independent legacy-v1 encoder plus old-to-current commit fault model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "txn.h"
#include "package.h"
static uint8_t stable[4096*512],cache[4096*512],seed[4096*512],baseline[4096*512],dirty[4096];
static SfTxn volume,recovered;
static unsigned checks,operations,fail_at,mode;
static const char*paths[]={"/notes/user","/slots/a.sfk","/slots/b.sfk","/boot/current"};
static uint32_t lengths[4];static uint8_t values[4][SF_TXN_FILE_BYTES];
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"TXN COMPAT FAIL line %d op %u mode %u\n",__LINE__,fail_at,mode);exit(1);}}while(0)
static void p32(uint8_t*p,uint32_t n){memcpy(p,&n,4);}
static void p64(uint8_t*p,uint64_t n){memcpy(p,&n,8);}
static uint32_t u32(const uint8_t*p){uint32_t n;memcpy(&n,p,4);return n;}
static uint8_t*bank(unsigned n){return stable+(SF_TXN_BANK+n*SF_TXN_BANK_SECTORS)*512;}
static uint8_t*record(unsigned n){return stable+(SF_TXN_HEADER+1+n)*512;}
static void reseal(unsigned n){uint8_t*r=record(n);sf_sha256(r+40,bank(n),SF_TXN_BANK_SECTORS*512);sf_sha256(r+480,r,480);}
/* A separate wire encoder: no calls to the current transaction encoder. */
static void encode(unsigned slot,uint64_t generation,unsigned count,int legacy){
 unsigned slots=legacy?4:5,data=legacy?1024:1152;CHECK(count<=4);
 uint8_t*b=bank(slot),*r=record(slot);memset(b,0,SF_TXN_BANK_SECTORS*512);memset(r,0,512);
 memcpy(b,"SFBANK1",8);p64(b+8,generation);p32(b+16,count);
 for(unsigned i=0;i<count;i++){uint8_t*e=b+512+i*128;memcpy(e,paths[i],strlen(paths[i])+1);p32(e+64,lengths[i]);memcpy(b+data+i*16384,values[i],lengths[i]);sf_sha256(e+96,b+data+i*16384,lengths[i]);}
 CHECK(data==512+slots*128);memcpy(r,"SFCMT1\0",8);p64(r+8,generation);p32(r+16,slot);memcpy(r+24,stable+SF_TXN_HEADER*512+16,16);reseal(slot);
}
static void clear(void){memcpy(stable,seed,sizeof(stable));memset(stable+(SF_TXN_HEADER+1)*512,0,1024);fail_at=mode=operations=0;}
static int rd(void*c,uint64_t lba,uint8_t out[512]){CHECK(lba<4096);memcpy(out,cache+lba*512,512);return 0;}
static void subset(void){for(unsigned i=0;i<4096;i++)if(dirty[i]&&((i*17+fail_at)%3)==0)memcpy(stable+i*512,cache+i*512,512);}
static int wr(void*c,uint64_t lba,const uint8_t in[512]){
 CHECK(lba>=SF_TXN_HEADER+1&&lba<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);
 if(++operations==fail_at){if(mode){subset();memcpy(stable+lba*512,in,mode==1?257:512);}return SF_DEVICE_GONE;}
 memcpy(cache+lba*512,in,512);dirty[lba]=1;return 0;
}
static int flush(void*c){
 if(++operations==fail_at){if(mode==1)subset();if(mode==2)memcpy(stable,cache,sizeof(stable));return SF_DEVICE_GONE;}
 memcpy(stable,cache,sizeof(stable));memset(dirty,0,sizeof(dirty));return 0;
}
static int mount(SfTxn*t){memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));return sf_txn_mount(t,rd,wr,flush,0);}
static void contents(SfTxn*t,unsigned count){
 static uint8_t out[SF_TXN_FILE_BYTES];
 for(unsigned i=0;i<count;i++){memset(out,0xa5,sizeof(out));CHECK(sf_txn_get(t,paths[i],out,sizeof(out))==(long)lengths[i]);CHECK(!memcmp(out,values[i],lengths[i]));}
 for(unsigned i=count;i<4;i++)CHECK(sf_txn_get(t,paths[i],out,sizeof(out))==SF_NOT_FOUND);
}
static void upgraded(SfTxn*t,int newer){
 uint8_t out[SF_TXN_FILE_BYTES];const char text[]="migrated";
 CHECK(t->generation==(uint64_t)(newer?8:7));
 CHECK(sf_txn_get(t,paths[0],out,sizeof(out))==(long)(newer?sizeof(text)-1:lengths[0]));
 CHECK(!memcmp(out,newer?(const void*)text:values[0],newer?sizeof(text)-1:lengths[0]));
 for(unsigned i=1;i<4;i++){CHECK(sf_txn_get(t,paths[i],out,sizeof(out))==(long)lengths[i]);CHECK(!memcmp(out,values[i],lengths[i]));}
 long n=sf_txn_get(t,"/boot/revoked",out,sizeof(out));CHECK(n==(newer?32:SF_NOT_FOUND));if(newer)for(unsigned i=0;i<32;i++)CHECK(out[i]==(uint8_t)(i+1));
}
int main(void){
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(seed,1,sizeof(seed),f)==sizeof(seed));fclose(f);
 const uint32_t sizes[]={0,1,127,128,129,16383,16384};
 for(unsigned round=0;round<1000;round++){
  unsigned count=round%5;for(unsigned i=0;i<4;i++){lengths[i]=sizes[(round+i)%7];for(unsigned j=0;j<lengths[i];j++)values[i][j]=(uint8_t)(round*31+i*17+j);}
  clear();encode(0,round+2,count,1);memcpy(baseline,stable,sizeof(stable));CHECK(!mount(&volume));CHECK(volume.generation==round+2);contents(&volume,count);CHECK(!operations&&!memcmp(stable,baseline,sizeof(stable)));
  /* Highest-generation selection works in either direction across profiles. */
  encode(1,round+3,count,0);CHECK(!mount(&volume)&&volume.generation==round+3);contents(&volume,count);
  encode(0,round+4,count,1);CHECK(!mount(&volume)&&volume.generation==round+4);contents(&volume,count);
 }
 for(unsigned i=0;i<4;i++){lengths[i]=19+i;for(unsigned j=0;j<lengths[i];j++)values[i][j]=(uint8_t)(i*31+j+1);}
 /* Repaired outer hashes do not permit noncanonical old-bank contents. */
 const unsigned bad[]={20,512+68,512+96,512,512+128,1024+19,1024+4*16384,SF_TXN_BANK_SECTORS*512-1};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){
  clear();encode(0,10,4,1);encode(1,9,4,0);bank(0)[bad[i]]^=0x7f;reseal(0);CHECK(!mount(&volume)&&volume.generation==9);contents(&volume,4);
 }
 clear();encode(0,10,4,1);encode(1,9,4,0);memcpy(bank(0)+512+128,bank(0)+512,64);reseal(0);CHECK(!mount(&volume)&&volume.generation==9);
 clear();encode(0,10,1,1);encode(1,9,4,0);bank(0)[512+128]=1;reseal(0);CHECK(!mount(&volume)&&volume.generation==9);
 clear();encode(0,10,4,1);encode(1,10,4,0);CHECK(mount(&volume)==SF_BAD_FORMAT&&!volume.ready);
 clear();encode(0,10,4,1);encode(1,10,4,1);CHECK(mount(&volume)==SF_BAD_FORMAT&&!volume.ready);
 clear();encode(0,7,4,1);encode(1,6,4,1);memcpy(baseline,stable,sizeof(stable));CHECK(!mount(&volume));
 uint8_t id[32];for(unsigned i=0;i<32;i++)id[i]=i+1;
 const char*changed[]={"/notes/user","/boot/revoked"};const void*data[]={"migrated",id};uint32_t bytes[]={8,32};
 operations=0;CHECK(!sf_txn_commit(&volume,changed,data,bytes,2));unsigned total=operations;CHECK(total==261);CHECK(!mount(&recovered));upgraded(&recovered,1);
 CHECK(u32(bank(1)+16)==5);CHECK(!memcmp(stable,baseline,SF_TXN_HEADER*512));
 CHECK(!memcmp(bank(0),baseline+SF_TXN_BANK*512,SF_TXN_BANK_SECTORS*512));CHECK(!memcmp(record(0),baseline+(SF_TXN_HEADER+1)*512,512));
 for(mode=0;mode<3;mode++)for(fail_at=1;fail_at<=total;fail_at++){
  memcpy(stable,baseline,sizeof(stable));CHECK(!mount(&volume));operations=0;
  CHECK(sf_txn_commit(&volume,changed,data,bytes,2)==SF_COMMIT_UNCERTAIN&&!volume.ready);
  CHECK(sf_txn_get(&volume,paths[0],id,sizeof(id))==SF_DEVICE_GONE);CHECK(!mount(&recovered));CHECK(recovered.generation==7||recovered.generation==8);upgraded(&recovered,recovered.generation==8);
 }
 printf("TXN COMPAT PASS: %u checks; 1000 legacy/mixed snapshots, strict padding/hash/path validation, read-only mount, five-key upgrade and %u first-upgrade fault scenarios\n",checks,total*3);return 0;
}
