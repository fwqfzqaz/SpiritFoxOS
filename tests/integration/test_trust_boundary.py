"""Compile the real Root package loader and prove authority faults precede I/O."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from test_file_ingress import function

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import build

PREFIX=r'''
#include <stdio.h>
#include "sf.h"
#include "package.h"
#include "persistence.h"
static int fault,bad;
static unsigned queries,maps,reads,bundles,auth;
static uint8_t scratch[1];
static const SfBootArgs args={.bundle=0x55,.bundle_bytes=9};
static const SfBootArgs*boot=&args;
SfRuntimeTrust sf_trust;
int sf_persist_security_error(void){queries++;return fault;}
static void*stage_grow(uint64_t*base,uint64_t*have,uint64_t bytes){(void)base;(void)have;if(bytes!=SF_STORAGE_SNAPSHOT_BYTES)bad=1;maps++;return scratch;}
long sf_storage_load(const char*name,void*out,size_t bytes){(void)name;if(out!=scratch||bytes!=SF_STORAGE_SNAPSHOT_BYTES)bad=1;reads++;return SF_NOT_FOUND;}
long sf_persist_package(void*out,size_t bytes){if(out!=scratch||bytes!=SF_STORAGE_SNAPSHOT_BYTES)bad=1;reads++;return SF_NOT_FOUND;}
int sf_bundle_find(const void*data,uint64_t bytes,const char*name,const uint8_t**out,uint64_t*length){(void)name;(void)out;(void)length;if(data!=(void*)0x55||bytes!=9)bad=1;bundles++;return SF_NOT_FOUND;}
int sf_trust_open(const SfRuntimeTrust*s,SfPackage*p,const void*d,uint64_t n){(void)s;(void)p;(void)d;(void)n;auth++;return SF_BAD_SIGNATURE;}
'''

MAIN=r'''
static unsigned checks;
#define CHECK(c) do{checks++;if(!(c)){printf("TRUST ADMISSION FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 const char*paths[]={"hello.sfk","sf-native.sfmod","/disk/bin/app.sfk","/installed.sfk"};
 const int errors[]={SF_BAD_FORMAT,SF_NO_SPACE,SF_DEVICE_GONE,SF_COMMIT_UNCERTAIN};
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<4;j++){
  queries=maps=reads=bundles=auth=0;bad=0;fault=errors[j];uint64_t base=0x99,have=0x33;SfPackage p,before;memset(&p,0xa5,sizeof(p));before=p;
  CHECK(package_get(paths[i],&p,&base,&have)==fault);
  CHECK(queries==1&&!maps&&!reads&&!bundles&&!auth&&!bad);
  CHECK(base==0x99&&have==0x33);CHECK(!memcmp(&p,&before,sizeof(p)));
 }
 for(unsigned i=0;i<4;i++){
  queries=maps=reads=bundles=auth=0;bad=0;fault=0;uint64_t base=0,have=0;SfPackage p;
  CHECK(package_get(paths[i],&p,&base,&have)==SF_NOT_FOUND);CHECK(queries==1&&!auth&&!bad);
  CHECK(paths[i][0]=='/'?(maps==1&&reads==1&&!bundles):(!maps&&!reads&&bundles==1));
 }
 printf("TRUST ADMISSION PASS: %u checks; actual package_get, module/app/bundle/disk/installed and no pre-admission I/O\n",checks);return 0;
}
'''


class TrustBoundary(unittest.TestCase):
    def test_root_checks_runtime_trust_before_package_io(self):
        source=(ROOT/'services/init.c').read_text(encoding='utf-8')
        code=PREFIX+function(source,'static int package_get(')+'\n'+MAIN
        with tempfile.TemporaryDirectory(prefix='trust-boundary-',dir=ROOT/'build') as name:
            folder=Path(name).resolve();self.assertTrue(folder.is_relative_to(ROOT/'build'))
            path=folder/'actual.c';binary=folder/('actual'+build.EXE)
            path.write_text(code,encoding='utf-8',newline='\n')
            build.run(['-target',build.HOST,'-g0','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DSF_HOST_TEST','-Iinclude',str(path.relative_to(ROOT)),'-o',str(binary.relative_to(ROOT))])
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertRegex(result.stdout,r'TRUST ADMISSION PASS: 76 checks')


if __name__=='__main__':unittest.main(verbosity=2)
