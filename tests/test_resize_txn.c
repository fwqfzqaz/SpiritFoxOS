/* Exercise the actual resize adapter and transaction engine across every
 * write/flush boundary: shrink, extend, clear, and absent-file extension. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "txn.h"
static uint8_t stable[4096*512],cache[4096*512],original[4096*512],baseline[4096*512],dirty[4096];
static SfTxn volume,recovered;static unsigned checks;static int operations,fail_at,mode;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"RESIZE FAIL line %d boundary %d mode %d\n",__LINE__,fail_at,mode);exit(1);}}while(0)
static int read_sector(void*c,uint64_t lba,uint8_t out[512]){if(lba>=4096)return SF_INVALID;memcpy(out,cache+lba*512,512);return 0;}
static void subset(void){for(unsigned i=0;i<4096;i++)if(dirty[i]&&((i*17+fail_at)%3)==0)memcpy(stable+i*512,cache+i*512,512);}
static int write_sector(void*c,uint64_t lba,const uint8_t in[512]){CHECK(lba>SF_TXN_HEADER&&lba<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);if(++operations==fail_at){if(mode){subset();memcpy(stable+lba*512,in,mode==1?257:512);}return SF_DEVICE_GONE;}memcpy(cache+lba*512,in,512);dirty[lba]=1;return 0;}
static int flush(void*c){if(++operations==fail_at){if(mode==1)subset();if(mode==2)memcpy(stable,cache,sizeof(stable));return SF_DEVICE_GONE;}memcpy(stable,cache,sizeof(stable));memset(dirty,0,sizeof(dirty));return 0;}
static void boot(SfTxn*t){memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));CHECK(!sf_txn_mount(t,read_sector,write_sector,flush,0));}
static void guards(SfTxn*t){const char*paths[]={"/boot/current","/slots/a.sfk","/slots/b.sfk"};for(unsigned i=0;i<3;i++){char out[8];CHECK(sf_txn_get(t,paths[i],out,sizeof(out))==5&&!memcmp(out,"guard",5));}}
static void content(SfTxn*t,unsigned old,unsigned next,int absent){
 uint8_t out[SF_TXN_FILE_BYTES];long n=sf_txn_get(t,"/notes/user",out,sizeof(out));const char*prefix=old==4?"keep":"secret-tail-data";
 if(t->generation==3){CHECK(n==(absent?SF_NOT_FOUND:(long)old));if(!absent)CHECK(!memcmp(out,prefix,old));}
 else{CHECK(t->generation==4&&n==(long)next);unsigned kept=old<next?old:next;if(absent)kept=0;CHECK(!memcmp(out,prefix,kept));for(unsigned i=kept;i<next;i++)CHECK(out[i]==0);}
 guards(t);
}
int main(void){
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(original,1,sizeof(original),f)==sizeof(original));fclose(f);
 unsigned scenarios=0;for(unsigned transition=0;transition<4;transition++){
  unsigned old=transition==1?4:16,next=transition==0?4:transition==2?0:20;int absent=transition==3;
  memcpy(stable,original,sizeof(stable));fail_at=mode=operations=0;boot(&volume);
  const char*paths[]={"/boot/current","/slots/a.sfk","/slots/b.sfk","/notes/user"};const void*data[]={"guard","guard","guard",old==4?"keep":"secret-tail-data"};uint32_t sizes[]={5,5,5,old};
  CHECK(!sf_txn_commit(&volume,paths,data,sizes,absent?3:4));CHECK(!sf_txn_commit(&volume,paths,data,sizes,absent?3:4));CHECK(volume.generation==3);memcpy(baseline,stable,sizeof(stable));
  operations=0;CHECK(!sf_txn_resize(&volume,"/notes/user",absent?0:old));CHECK(!operations&&!memcmp(stable,baseline,sizeof(stable)));
  CHECK(!sf_txn_resize(&volume,"/notes/user",next));CHECK(operations==261);boot(&recovered);content(&recovered,old,next,absent);
  for(mode=0;mode<3;mode++)for(fail_at=1;fail_at<=261;fail_at++){
   memcpy(stable,baseline,sizeof(stable));boot(&volume);operations=0;CHECK(sf_txn_resize(&volume,"/notes/user",next)==SF_COMMIT_UNCERTAIN);CHECK(!volume.ready);CHECK(sf_txn_resize(&volume,"/notes/user",0)==SF_DEVICE_GONE);boot(&recovered);content(&recovered,old,next,absent);scenarios++;
  }
 }
 fail_at=mode=0;memcpy(stable,baseline,sizeof(stable));boot(&volume);CHECK(!sf_txn_resize(&volume,"/notes/user",SF_TXN_FILE_BYTES));uint8_t out[SF_TXN_FILE_BYTES];CHECK(sf_txn_get(&volume,"/notes/user",out,sizeof(out))==SF_TXN_FILE_BYTES);for(unsigned i=0;i<sizeof(out);i++)CHECK(out[i]==0);
 operations=0;uint64_t generation=volume.generation;CHECK(sf_txn_resize(&volume,"/notes/user",SF_TXN_FILE_BYTES+1)==SF_NO_SPACE);CHECK(sf_txn_resize(&volume,"/notes/user",UINT64_MAX)==SF_NO_SPACE);CHECK(sf_txn_resize(&volume,"/../bad",0)==SF_INVALID);CHECK(sf_txn_resize(&volume,"/no-slot",1)==SF_NO_SPACE);CHECK(!operations&&volume.generation==generation&&volume.ready);
 CHECK(!sf_txn_resize(&volume,"/notes/user",0));CHECK(!sf_txn_resize(&volume,"/notes/user",SF_TXN_FILE_BYTES));boot(&recovered);CHECK(sf_txn_get(&recovered,"/notes/user",out,sizeof(out))==SF_TXN_FILE_BYTES);for(unsigned i=0;i<sizeof(out);i++)CHECK(out[i]==0);guards(&recovered);
 printf("RESIZE TXN PASS: %u checks, %u fault scenarios (4 transitions x 261 boundaries x 3 modes); zero extension, idempotence and independent files preserved\n",checks,scenarios);return 0;
}
