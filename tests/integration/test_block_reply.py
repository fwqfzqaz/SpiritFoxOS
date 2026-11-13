"""Exercise both production block clients and their actual sector-copy loops."""
import json
from pathlib import Path
import subprocess
import sys
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build
from test_file_ingress import function

PREFIX=r'''
#include <stdio.h>
#include <stdlib.h>
#include "block_rpc.h"
static uint32_t driver_ep=0x101,driver_reply=0x102,driver_pid=12;
static uint64_t capacity=4096,sequence;
static int driver_alive __attribute__((unused)),running,variant,bad_after,send_error,recv_error,service_error;
static unsigned checks,sends,receives,closed,killed;
static SfMessage wire;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d case%d: %s\n",__LINE__,variant,#x);exit(1);}}while(0)
/* Preserve the guest's LP64 sentinel on an LLP64 Windows test host. */
static int64_t sf_sys(unsigned n,uint64_t a,uint64_t b,uint64_t c){(void)a;(void)b;(void)c;
 if(n==SYS_TASK_STATUS)return running?SF_TASK_RUNNING:SF_DEVICE_GONE;
 if(n==SYS_KILL){killed++;running=0;}return 0;}
static long sf_cap_close(uint64_t cap){CHECK(cap==0x888);closed++;return 0;}
static uint64_t sf_deadline(unsigned ms){return 10000000ull+ms;}
static void sf_message(SfMessage*m,unsigned op,uint64_t id){memset(m,0,sizeof(*m));m->h.size=40;m->h.protocol_major=1;m->h.opcode=op;m->h.request_id=id;m->h.payload_bytes=sizeof(*m)-40;m->h.deadline_ns=sf_deadline(3000);}
static int effective(void){return sends>(unsigned)bad_after?variant:0;}
static long sf_send(uint32_t ep,SfMessage*m){
 CHECK(ep==driver_ep);sends++;wire=*m;wire.h.flags=driver_pid;
 memset(wire.data,0x40+wire.mode/256,256);
 if(m->h.opcode==OP_BLOCK_INFO)wire.length=sizeof(SfBlockInfo);
 if(service_error){wire.status=service_error;wire.length=0;}
 switch(effective()){
 case 1:wire.h.opcode=OP_BLOCK_FLUSH;break;
 case 2:wire.status=1ll<<32;break;
 case 3:wire.h.flags=99;break;
 case 4:wire.h.size=0;break;
 case 5:wire.h.protocol_major=2;break;
 case 6:wire.h.payload_bytes--;break;
 case 7:wire.h.deadline_ns++;break;
 case 8:wire.h.cap_count=1;wire.handle=0x888;break;
 case 9:wire.mode^=256;break;
 case 10:wire.offset+=512;break;
 case 11:wire.handle++;break;
 case 12:wire.length=1;break;
 case 13:wire.status=-(1ll<<32);break;
 case 14:wire.h.request_id++;break;
 case 16:wire.status=SF_INVALID;wire.length=1;break;
 case 17:wire.h.cap_count=2;break;
 case 18:wire.h.protocol_minor=1;break;
 case 19:wire.status=INT64_MIN;break;
 case 20:wire.status=1;break;
 }
 return send_error;
}
static long sf_recv(uint32_t ep,SfMessage*m,uint64_t deadline){CHECK(ep==driver_reply&&deadline==sf_deadline(2000));receives++;if(recv_error)return recv_error;*m=wire;return effective()==15?driver_pid+1:driver_pid;}
static void reset(void){driver_ep=0x101;driver_reply=0x102;driver_pid=12;driver_alive=running=1;sequence=0x200000000ull;sends=receives=closed=killed=0;variant=bad_after=send_error=recv_error=service_error=0;}
static void failed(void){
#if OWNER_STORAGE
 CHECK(killed==1&&!running);
#else
 CHECK(!driver_alive&&!killed);
#endif
}
'''
MAIN=r'''
int main(void){
 const unsigned ops[]={OP_BLOCK_INFO,OP_BLOCK_READ,OP_BLOCK_WRITE,OP_BLOCK_FLUSH,OP_BLOCK_WRITE_RANGE,OP_BLOCK_STOP,OP_BLOCK_DMA_PROBE};
 for(unsigned i=0;i<sizeof(ops)/sizeof(*ops);i++){
  reset();SfMessage m;sf_message(&m,ops[i],0);if(ops[i]==OP_BLOCK_READ||ops[i]==OP_BLOCK_WRITE)m.length=256;
  m.offset=512;m.mode=256;m.handle=4096;CHECK(!call(&m));CHECK(sends==1&&receives==1&&!closed&&!killed&&running&&driver_alive);
 }
 for(variant=1;variant<=20;){
  int v=variant;reset();variant=v;SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.length=256;
  CHECK(call(&m)==((v==14||v==15)?SF_STALE_HANDLE:SF_BAD_FORMAT));CHECK(sends==1&&receives==1&&closed==(v==8?1u:0u));failed();variant=v+1;
 }
 reset();sequence=UINT64_MAX;SfMessage m;sf_message(&m,OP_BLOCK_READ,0);m.length=256;SfMessage before=m;
 CHECK(call(&m)==SF_NO_SPACE);CHECK(!memcmp(&m,&before,sizeof(m))&&sequence==UINT64_MAX&&!sends&&!closed&&!killed&&running&&driver_alive);
 for(int error=SF_VERSION_REJECTED;error<0;error++){
  reset();sf_message(&m,OP_BLOCK_READ,0);m.length=256;service_error=error;CHECK(call(&m)==error);CHECK(sends==1&&receives==1&&!closed&&!killed&&running&&driver_alive);
  reset();sf_message(&m,OP_BLOCK_FLUSH,0);send_error=error;CHECK(call(&m)==error);CHECK(sends==1&&!receives&&!closed&&!killed&&running&&driver_alive);
  reset();sf_message(&m,OP_BLOCK_FLUSH,0);recv_error=error;CHECK(call(&m)==error);CHECK(sends==1&&receives==1&&!closed);failed();
 }
 reset();sf_message(&m,OP_BLOCK_FLUSH,0);send_error=1;CHECK(call(&m)==SF_BAD_FORMAT);CHECK(!receives);failed();
 /* Copy actual512-byte sectors with guards, then reject a bad first/second
  * half without copying that half; a verified first prefix may remain. */
 for(unsigned stage=0;stage<3;stage++){
  reset();if(stage){variant=1;bad_after=stage==2?1:0;}
  unsigned char out[528];memset(out,0xa7,sizeof(out));int r=SECTOR(0,0,out+8);
  CHECK(r==(stage?SF_BAD_FORMAT:0));
  for(unsigned i=0;i<sizeof(out);i++){
   unsigned char expected=0xa7;
   if(!stage&&i>=8&&i<520)expected=i<264?0x40:0x41;
   if(stage==2&&i>=8&&i<264)expected=0x40;
   CHECK(out[i]==expected);
  }
 }
 printf("BLOCK CLIENT CHECKS %u\n",checks);return 0;
}
'''

class BlockReply(unittest.TestCase):
    def test_actual_root_and_persist_calls(self):
        folder=ROOT/'build/block-reply-host';folder.mkdir(exist_ok=True)
        results=[]
        for owner,sector in [('persist','sector_read'),('storage','sector')]:
            source=(ROOT/f'services/{owner}.c').read_text(encoding='utf-8')
            body=PREFIX+function(source,'static int call(')+'\n'+function(source,f'static int {sector}(')
            c=folder/f'{owner}.c';binary=folder/(owner+build.EXE)
            c.write_text(f'#define OWNER_STORAGE {int(owner=="storage")}\n#define SECTOR {sector}\n'+body+'\n'+MAIN,encoding='utf-8',newline='\n')
            build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Iinclude','-Iservices',str(c.relative_to(ROOT)),'-o',str(binary.relative_to(ROOT))])
            run=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            results.append({'owner':owner,'checks':int(run.stdout.strip().split()[-1])})
        (ROOT/'build/block-reply-host-results.json').write_text(json.dumps({'passed':True,'clients':results,'checks':sum(r['checks'] for r in results)},indent=2)+'\n',encoding='utf-8')

if __name__=='__main__':unittest.main(verbosity=2)
