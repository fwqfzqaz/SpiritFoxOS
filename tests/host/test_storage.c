#include <stdio.h>
#include "namespace.h"
#include "virtio.h"
#include "../../services/namespace_state.h"
static SfNamespace ns;static unsigned leases,count;static int admission=1;
#define CHECK(x) do{count++;if(!(x)){printf("FAIL storage line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int mount_open(unsigned tag){if(tag!=1)return SF_INVALID;if(!admission)return SF_DRAINING;leases++;return 0;}
static void mount_close(unsigned tag){if(tag==1)leases--;}
static long external(unsigned index,uint64_t offset,void*out,size_t bytes){if(index!=7)return SF_INVALID;if(offset>=4096)return 0;if(bytes>4096-offset)bytes=4096-offset;memset(out,'a'+offset%26,bytes);return bytes;}
static uint8_t fake[4096*4];static int fake_fail,fake_reads,fake_writes;
static int fake_io(void*context,int write,uint64_t lba){
 SfBlockBridge*b=context;if(fake_fail)return SF_DEVICE_GONE;if(lba>=4)return SF_INVALID;
 if(write){fake_writes++;memcpy(fake+lba*b->lba_bytes,b->cache,b->lba_bytes);}
 else{fake_reads++;memcpy(b->cache,fake+lba*b->lba_bytes,b->lba_bytes);}
 return 0;
}
static int same(const void*a,const void*b,size_t bytes){const uint8_t*x=a,*y=b;for(size_t i=0;i<bytes;i++)if(x[i]!=y[i])return 0;return 1;}
int main(void){
 uint32_t used;CHECK(!sf_virtq_layout(256,&used)&&used==8192);CHECK(!sf_virtq_layout(8,&used)&&used==4096);CHECK(sf_virtq_layout(0,&used)==SF_BAD_FORMAT);CHECK(sf_virtq_layout(3,&used)==SF_BAD_FORMAT);CHECK(sf_virtq_layout(512,&used)==SF_BAD_FORMAT);
 uint16_t identify[256]={0};uint32_t lba_bytes=0;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==512);
 identify[106]=1u<<12;identify[117]=2048;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==4096);
 identify[117]=1024;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==2048);
 identify[117]=256;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==512);
 identify[117]=0;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==512);
 identify[117]=0xffff;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==512);
 identify[117]=4096;CHECK(sf_ata_logical_bytes(identify,&lba_bytes)==SF_BAD_FORMAT);
 identify[117]=2048;identify[118]=1;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==4096);
 memset(identify,0,sizeof(identify));identify[117]=300;CHECK(sf_ata_logical_bytes(identify,&lba_bytes)==SF_BAD_FORMAT);
 identify[106]=0;identify[117]=256;CHECK(!sf_ata_logical_bytes(identify,&lba_bytes)&&lba_bytes==512);
 uint64_t lba=0;uint32_t within=0;
 CHECK(!sf_lba_split(0,512,4096,&lba,&within)&&!lba&&!within);
 CHECK(sf_lba_split(256,512,4096,&lba,&within)==SF_INVALID&&!lba&&!within);
 CHECK(!sf_lba_split(512*3,4096,4096,&lba,&within)&&!lba&&within==1536);
 CHECK(!sf_lba_split(4096,4096,4096,&lba,&within)&&lba==1&&!within);
 CHECK(!sf_lba_split(4096,1024,4096,&lba,&within)&&lba==4&&!within);
 CHECK(sf_lba_split(1,4096,4096,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(4096,4096,4096+4,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(4096*512,4096,4096,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(4096,1024,4096+1,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(UINT64_MAX,4096,4096,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(0,256,4096,&lba,&within)==SF_INVALID);
 CHECK(sf_lba_split(0,8192,4096,&lba,&within)==SF_INVALID);
 CHECK(sf_block_range(2048,0,0,256));CHECK(sf_block_range(2048,2047*512,256,256));CHECK(!sf_block_range(2048,2048*512,0,256));CHECK(!sf_block_range(2048,1,0,256));CHECK(!sf_block_range(2048,0,512,256));CHECK(!sf_block_range(2048,0,1,256));CHECK(!sf_block_range(2048,0,0,257));CHECK(!sf_block_range(2048,UINT64_MAX,0,256));
 SfBlockBridge bridge;uint8_t half0[256],half1[256],out[256],before[sizeof(fake)];
 for(unsigned i=0;i<256;i++){half0[i]=(uint8_t)(0x40+i);half1[i]=(uint8_t)(0x80+i);}
 for(unsigned i=0;i<sizeof(fake);i++)fake[i]=(uint8_t)((i*37+11)&255);
 memcpy(before,fake,sizeof(fake));
 /* 4K logical block: a 512-byte write reads the block, merges only that sector and writes back. */
 sf_bridge_init(&bridge,&bridge,fake_io,4096);fake_reads=fake_writes=0;
 CHECK(!sf_bridge_write(&bridge,4096+1024,32,0,half0));CHECK(!sf_bridge_write(&bridge,4096+1024,32,256,half1));
 CHECK(fake_reads==1&&fake_writes==1&&same(fake+4096+1024,half0,256)&&same(fake+4096+1280,half1,256));
 CHECK(same(fake,before,4096+1024)&&same(fake+4096+1536,before+4096+1536,4096-1536));
 CHECK(!sf_bridge_read(&bridge,4096+1024,32,0,out)&&same(out,half0,256)&&fake_reads==1);
 CHECK(!sf_bridge_read(&bridge,4096+1024,32,256,out)&&same(out,half1,256)&&fake_reads==1);
 CHECK(sf_bridge_read(&bridge,4096*4,32,0,out)==SF_INVALID);
 CHECK(sf_bridge_read(&bridge,3,32,0,out)==SF_INVALID);
 CHECK(!sf_bridge_write(&bridge,4096+1024,32,0,half0));
 CHECK(sf_bridge_write(&bridge,4096+512,32,256,half1)==SF_INVALID);
 CHECK(!sf_bridge_read(&bridge,0,32,0,out)&&fake_reads==2);
 /* A failed read-modify-write read discards the half block and can be retried. */
 sf_bridge_init(&bridge,&bridge,fake_io,4096);memcpy(before,fake,sizeof(fake));fake_fail=1;fake_reads=fake_writes=0;
 CHECK(!sf_bridge_write(&bridge,2048,32,0,half0));CHECK(sf_bridge_write(&bridge,2048,32,256,half1)==SF_DEVICE_GONE);
 fake_fail=0;CHECK(!sf_bridge_write(&bridge,2048,32,0,half0));CHECK(!sf_bridge_write(&bridge,2048,32,256,half1));
 CHECK(fake_reads==1&&fake_writes==1&&same(fake+2048,half0,256)&&same(fake+2304,half1,256));
 CHECK(same(fake,before,2048)&&same(fake+2560,before+2560,4096-2560));
 /* 512-byte devices write the assembled sector directly without a read. */
 sf_bridge_init(&bridge,&bridge,fake_io,512);fake_reads=fake_writes=0;
 CHECK(!sf_bridge_write(&bridge,512,32,0,half0));CHECK(!sf_bridge_write(&bridge,512,32,256,half1));
 CHECK(!fake_reads&&fake_writes==1&&same(fake+512,half0,256)&&same(fake+768,half1,256));
 CHECK(!sf_bridge_read(&bridge,512,32,0,out)&&same(out,half0,256)&&fake_reads==0);
 sf_bridge_init(&bridge,&bridge,fake_io,512);
 CHECK(!sf_bridge_read(&bridge,512,32,256,out)&&same(out,half1,256)&&fake_reads==1);
 /* FLUSH discards a lone half block, so the next write must start a fresh pair. */
 sf_bridge_init(&bridge,&bridge,fake_io,512);fake_reads=fake_writes=0;
 CHECK(!sf_bridge_write(&bridge,512,32,0,half0));sf_bridge_flush(&bridge);CHECK(sf_bridge_write(&bridge,512,32,256,half1)==SF_INVALID);
 CHECK(!sf_bridge_write(&bridge,512,32,0,half0)&&!sf_bridge_write(&bridge,512,32,256,half1));
 CHECK(!fake_reads&&fake_writes==1&&same(fake+512,half0,256)&&same(fake+768,half1,256));
 sf_ns_init(&ns,0);CHECK(!sf_ns_create(&ns,"/disk",NODE_DIR,0));CHECK(!sf_ns_create(&ns,"/disk/item",NODE_DISK,1));ns.external_read=external;ns.mount_open=mount_open;ns.mount_close=mount_close;
 for(unsigned i=0;i<SF_NS_NODE_MAX;i++)if(ns.nodes[i].type&&!strcmp(ns.nodes[i].path,"/disk")){ns.nodes[i].mount_tag=1;ns.nodes[i].readonly=1;}else if(ns.nodes[i].type==NODE_DISK){ns.nodes[i].mount_tag=1;ns.nodes[i].external=7;ns.nodes[i].size=4096;}
 int h=sf_ns_open(&ns,42,"/","/disk/item",1);CHECK(h>0&&leases==1);int dup=sf_ns_dup(&ns,42,h);CHECK(dup>0&&leases==1);CHECK(sf_ns_unmount(&ns,1)==SF_BUSY);CHECK(sf_ns_open(&ns,42,"/","/disk/item",2)==SF_READ_ONLY);CHECK(sf_ns_create(&ns,"/disk/new",NODE_FILE,0)==SF_READ_ONLY);
 char data[2048];CHECK(sf_ns_seek(&ns,42,h,2000)==2000);CHECK(sf_ns_read(&ns,42,dup,data,sizeof(data))==2048);CHECK(sf_ns_read(&ns,42,h,data,sizeof(data))==48);CHECK(!sf_ns_read(&ns,42,h,data,1));CHECK(sf_ns_seek(&ns,42,h,4097)==SF_INVALID);
 admission=0;CHECK(sf_ns_open(&ns,42,"/","/disk/item",1)==SF_DRAINING);CHECK(leases==1);CHECK(!sf_ns_close(&ns,42,h)&&leases==1);CHECK(!sf_ns_close(&ns,42,dup)&&leases==0);admission=1;
 int dir=sf_ns_open(&ns,42,"/","/disk",1);CHECK(dir>0&&leases==1);CHECK(sf_ns_unmount(&ns,1)==SF_BUSY);sf_ns_reap(&ns,42);CHECK(leases==0);CHECK(!sf_ns_unmount(&ns,1));CHECK(sf_ns_open(&ns,42,"/","/disk/item",1)==SF_NOT_FOUND);CHECK(sf_ns_read(&ns,42,h,data,1)==SF_STALE_HANDLE);
 printf("Storage service tests passed: %u assertions\n",count);return 0;
}
