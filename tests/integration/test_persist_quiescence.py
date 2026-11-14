"""Compile the owning Root storage guard with real module ledger definitions."""
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
#include <string.h>
#include "storage.h"
static SfManager state,*manager;
static int fs_slot,mounted,writable,driver_live,fs_live;
static uint32_t driver_pid,fs_pid,namespace_descriptions;
static struct {unsigned inflight;} fs_transport;
static int64_t sf_sys(unsigned n,uint64_t p,uint64_t b,uint64_t c){(void)b;(void)c;
 if(n!=SYS_TASK_STATUS)abort();return (p==driver_pid?driver_live:fs_live)?SF_TASK_RUNNING:SF_DEVICE_GONE;}
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reset(void){memset(&state,0,sizeof(state));manager=&state;fs_slot=2;mounted=writable=driver_live=fs_live=1;driver_pid=11;fs_pid=12;namespace_descriptions=fs_transport.inflight=0;state.modules[fs_slot].state=MOD_ACTIVE;}
static void observe(int expected){SfManager before=state;unsigned held=namespace_descriptions;CHECK(sf_storage_quiescent()==expected);CHECK(!memcmp(&before,&state,sizeof(state))&&held==namespace_descriptions);}
'''
MAIN=r'''
int main(void){
 reset();observe(0);
 for(unsigned resource=0;resource<RES_COUNT;resource++){
  const uint32_t counts[]={1,2,UINT32_MAX};
  for(unsigned i=0;i<3;i++){reset();state.modules[fs_slot].resources[resource]=counts[i];observe(SF_BUSY);}
 }
 reset();namespace_descriptions=1;observe(SF_BUSY);reset();fs_transport.inflight=1;observe(SF_BUSY);
 for(unsigned i=0;i<7;i++){
  reset();if(!i)manager=0;else if(i==1)fs_slot=-1;else if(i==2)mounted=0;else if(i==3)writable=0;else if(i==4)driver_live=0;else if(i==5)fs_live=0;else fs_pid=0;
  observe(SF_DEVICE_GONE);
 }
 for(unsigned phase=MOD_DISCOVERED;phase<=MOD_QUARANTINED;phase++){reset();state.modules[fs_slot].state=phase;observe(phase==MOD_ACTIVE?0:SF_DRAINING);}
 printf("PERSIST STORAGE GUARD PASS %u\n",checks);return 0;
}
'''

class PersistQuiescence(unittest.TestCase):
    def test_root_ledger_guard(self):
        folder=ROOT/'build/persist-quiescence-host';folder.mkdir(exist_ok=True)
        source=(ROOT/'services/storage.c').read_text(encoding='utf-8')
        c=folder/'guard.c';binary=folder/('guard'+build.EXE)
        c.write_text(PREFIX+function(source,'int sf_storage_writable(')+'\n'+function(source,'int sf_storage_quiescent(')+MAIN,encoding='utf-8',newline='\n')
        build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-Iinclude',str(c.relative_to(ROOT)),'-o',str(binary.relative_to(ROOT))])
        p=subprocess.run([str(binary)],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr)
        checks=int(p.stdout.strip().split()[-1]);self.assertGreater(checks,80)
        (ROOT/'build/persist-quiescence-guard-results.json').write_text(json.dumps({'passed':True,'checks':checks})+'\n',encoding='utf-8')

if __name__=='__main__':unittest.main(verbosity=2)
