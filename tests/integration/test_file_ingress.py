"""Compile the actual manager receive handler against a cap-install ledger."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build


def function(source,name):
    # Skip strings/comments while finding balanced braces, but compile the
    # original bytes, not a rewritten model of the handler.
    clean=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*',lambda m:' '*len(m[0]),source)
    start=clean.index(name);opening=clean.index('{',start);depth=0
    for i in range(opening,len(clean)):
        if clean[i]=='{':depth+=1
        elif clean[i]=='}':
            depth-=1
            if not depth:return source[start:i+1]
    raise ValueError('Incomplete production function '+name)


PREFIX=r'''
#include <stdio.h>
#include "namespace.h"
#include "emergency_console.h"
#include "file_rpc.h"
static unsigned checks,closed,sent,dispatched,broker_calls,bad,installed;
static long received;
static SfMessage wire,last_reply;
static SfNamespace *ns;
static uint32_t fs_ep=0x101,emergency_generation;
static long console_write(const void*p,size_t n){(void)p;return (long)n;}
static int sf_storage_broker(unsigned timeout){if(timeout!=1)bad++;broker_calls++;return 0;}
static uint64_t sf_deadline(unsigned ms){return 123+(uint64_t)ms;}
static long sf_recv(uint32_t ep,SfMessage*m,uint64_t deadline){
 if(ep!=fs_ep||deadline!=140)bad++;
 if(received<0)return received;
 *m=wire;if(m->h.cap_count==1)installed++;return received;
}
static long sf_cap_close(uint64_t h){
 if(h!=wire.handle||!installed)bad++;else installed--;
 closed++;return 0;
}
static long sf_send(uint32_t ep,SfMessage*m){if(ep!=0x302)bad++;sent++;last_reply=*m;return 0;}
static int sf_ns_client_healthy(void){return 1;}
int64_t sf_emergency_console(SfEmergencyConsole*c,uint32_t*g,uint32_t o,SfMessage*m,long(*write)(const void*,size_t)){
 (void)c;(void)g;(void)o;(void)m;(void)write;bad++;return SF_INVALID;
}
int64_t sf_file_dispatch_as(SfNamespace*n,uint32_t owner,uint32_t*root,SfMessage*m,uint32_t uid,uint32_t gid){
 if(n!=ns||owner!=42||*root!=0x404||uid!=1000||gid!=1001)bad++;
 dispatched++;m->status=0;m->length=0;return 0;
}
'''

MAIN=r'''
#define CHECK(x) do{checks++;if(!(x)){printf("FILE INGRESS FAIL line %d: %s; installed=%u closed=%u sent=%u\n",__LINE__,#x,installed,closed,sent);return 1;}}while(0)
int main(void){
 children[0]=(Child){42,0x302,0x404,1,1000,1001,{0,0}};
 for(unsigned known=0;known<2;known++)for(unsigned attached=0;attached<2;attached++)for(unsigned i=0;i<1000;i++){
  memset(&wire,0,sizeof(wire));closed=sent=dispatched=broker_calls=bad=installed=0;
  received=known?42:77;wire.h.flags=(uint32_t)received;wire.h.request_id=i+1;
  wire.h.size=40;wire.h.protocol_major=1;wire.h.payload_bytes=sizeof(wire)-40;
  wire.h.opcode=attached?OP_HELLO:OP_STAT;wire.h.cap_count=attached;
  wire.handle=0x501u+i*256u;wire.length=1;
  int status=serve_once(17);
  CHECK(status==(known?0:SF_INVALID));
  CHECK(closed==attached&&installed==0); /* receiver owns the installed cap */
  CHECK(sent==known&&dispatched==(known&&!attached));
  CHECK(!bad&&broker_calls==1);
  if(known){CHECK(last_reply.h.cap_count==0&&last_reply.h.request_id==i+1);CHECK(last_reply.status==(attached?SF_INVALID:0)&&!last_reply.length);}
 }
 closed=sent=dispatched=broker_calls=bad=installed=0;received=SF_TIMEOUT;
 CHECK(serve_once(17)==SF_TIMEOUT&&!closed&&!sent&&!dispatched&&!installed&&!bad);
 printf("FILE INGRESS PASS: %u checks; actual handler, installed caps, unknown/known peer, no-cap authority and receive errors\n",checks);return 0;
}
'''


class FileIngress(unittest.TestCase):
    def test_installed_caps_are_closed_even_for_unknown_peers(self):
        source=(ROOT/'services/init.c').read_text(encoding='utf-8')
        child=re.search(r'typedef struct \{[^\n]+\} Child;',source)
        self.assertIsNotNone(child)
        code=PREFIX+child[0]+'\nstatic Child children[SF_MAX_TASKS];\n'
        code+=function(source,'static Child*child(')+'\n'+function(source,'static int serve_once(')+'\n'+MAIN
        with tempfile.TemporaryDirectory(prefix='file-ingress-',dir=ROOT/'build') as temporary:
            folder=Path(temporary).resolve();self.assertTrue(folder.is_relative_to(ROOT/'build'))
            path=folder/'actual_ingress.c';binary=folder/('actual_ingress'+build.EXE)
            path.write_text(code,encoding='utf-8',newline='\n')
            build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-DSF_HOST_TEST','-Iservices','-Iinclude',str(path.relative_to(ROOT)),'-o',str(binary.relative_to(ROOT))])
            result=subprocess.run([str(binary)],text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertRegex(result.stdout,r'FILE INGRESS PASS: 20001 checks')


if __name__=='__main__':unittest.main(verbosity=2)
