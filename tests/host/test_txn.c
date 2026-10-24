/* Fault model: successful flush persists all previous writes. Before a flush,
 * any subset (including torn sectors) may reach durable media, in any order. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "txn.h"
#include "package.h"
static uint8_t stable[4096*512],cache[4096*512],baseline[4096*512];
static uint8_t dirty[4096];
static SfTxn volume,recovered;
static int operations,fail_at,mode,checks;
#define CHECK(c) do{checks++;if(!(c)){fprintf(stderr,"FAIL line %d op %d mode %d\n",__LINE__,fail_at,mode);exit(1);}}while(0)
static int read_sector(void*c,uint64_t lba,uint8_t out[512]){if(lba>=4096)return SF_INVALID;memcpy(out,cache+lba*512,512);return 0;}
static void persist_subset(void){for(unsigned i=0;i<4096;i++)if(dirty[i]&&((i*17+fail_at)%3)==0)memcpy(stable+i*512,cache+i*512,512);}
static int write_sector(void*c,uint64_t lba,const uint8_t in[512]){
 CHECK(lba>=SF_TXN_HEADER+1&&lba<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);
 if(++operations==fail_at){if(mode){persist_subset();memcpy(stable+lba*512,in,mode==1?257:512);}return SF_DEVICE_GONE;}
 memcpy(cache+lba*512,in,512);dirty[lba]=1;return 0;
}
static int flush(void*c){
 if(++operations==fail_at){if(mode==1)persist_subset();if(mode==2)memcpy(stable,cache,sizeof(stable));return SF_DEVICE_GONE;}
 memcpy(stable,cache,sizeof(stable));memset(dirty,0,sizeof(dirty));return 0;
}
static void boot(SfTxn*t){memcpy(cache,stable,sizeof(cache));memset(dirty,0,sizeof(dirty));CHECK(!sf_txn_mount(t,read_sector,write_sector,flush,0));}
int main(void){
 FILE*f=fopen("build/persistent.img","rb");CHECK(f&&fread(stable,1,sizeof(stable),f)==sizeof(stable));fclose(f);boot(&volume);
 const char*paths[]={"/notes/user","/boot/current"};const void*old[]={"old-data","a"},*next[]={"new-data","b"};uint32_t sizes[]={8,1};
 CHECK(!sf_txn_commit(&volume,paths,old,sizes,2));CHECK(!sf_txn_commit(&volume,paths,old,sizes,2));memcpy(baseline,stable,sizeof(stable));
 operations=0;CHECK(!sf_txn_commit(&volume,paths,next,sizes,2));int total=operations;CHECK(total==261);boot(&recovered);CHECK(recovered.generation==4);
 char text[8],selected;CHECK(sf_txn_get(&recovered,paths[0],text,8)==8&&!memcmp(text,"new-data",8));
 for(mode=0;mode<3;mode++)for(fail_at=1;fail_at<=total;fail_at++){
  memcpy(stable,baseline,sizeof(stable));boot(&volume);operations=0;
  CHECK(sf_txn_commit(&volume,paths,next,sizes,2)==SF_COMMIT_UNCERTAIN);CHECK(!volume.ready&&sf_txn_get(&volume,paths[0],text,8)==SF_DEVICE_GONE);
  boot(&recovered);CHECK(sf_txn_get(&recovered,paths[0],text,8)==8);CHECK(sf_txn_get(&recovered,paths[1],&selected,1)==1);
  CHECK((!memcmp(text,"old-data",8)&&selected=='a'&&recovered.generation==3)||(!memcmp(text,"new-data",8)&&selected=='b'&&recovered.generation==4));
 }
 fail_at=0;mode=0;memcpy(stable,baseline,sizeof(stable));boot(&volume);CHECK(!sf_txn_commit(&volume,paths,next,sizes,2));
 /* Damage newest bank: recover previous complete snapshot; damage both: deny. */
 stable[(SF_TXN_BANK+256)*512+SF_TXN_DATA]^=1;boot(&recovered);CHECK(recovered.generation==3);
 stable[SF_TXN_BANK*512+SF_TXN_DATA]^=1;memcpy(cache,stable,sizeof(cache));CHECK(sf_txn_mount(&recovered,read_sector,write_sector,flush,0)==SF_BAD_FORMAT&&!recovered.ready);
 memcpy(stable,baseline,sizeof(stable));boot(&volume);const char*invalid[]={"/../bad"};CHECK(sf_txn_commit(&volume,invalid,next,sizes,1)==SF_INVALID);
 const char*duplicate[]={"/same","/same"};CHECK(sf_txn_commit(&volume,duplicate,next,sizes,2)==SF_INVALID);sizes[0]=SF_TXN_FILE_BYTES+1;CHECK(sf_txn_commit(&volume,paths,next,sizes,1)==SF_INVALID);
 volume.generation=UINT64_MAX;CHECK(sf_txn_commit(&volume,paths,next,sizes,1)==SF_INVALID);
 /* Reserved geometry bytes are rejected even with a correct sector digest. */
 memcpy(stable,baseline,sizeof(stable));stable[SF_TXN_HEADER*512+32]=1;sf_sha256(stable+SF_TXN_HEADER*512+480,stable+SF_TXN_HEADER*512,480);memcpy(cache,stable,sizeof(cache));CHECK(sf_txn_mount(&recovered,read_sector,write_sector,flush,0)==SF_BAD_FORMAT);
 /* Two independently valid banks claiming the same generation are ambiguous. */
 memcpy(stable,baseline,sizeof(stable));uint64_t same=3;uint8_t*bank=stable+(SF_TXN_BANK+256)*512,*record=stable+(SF_TXN_HEADER+2)*512;
 memcpy(bank+8,&same,8);memcpy(record+8,&same,8);sf_sha256(record+40,bank,256*512);sf_sha256(record+480,record,480);memcpy(cache,stable,sizeof(cache));CHECK(sf_txn_mount(&recovered,read_sector,write_sector,flush,0)==SF_BAD_FORMAT);
 printf("TXN PASS: %d checks, %d operation boundaries x 3 fault modes; acknowledged commit and corruption recovery\n",checks,total);return 0;
}
