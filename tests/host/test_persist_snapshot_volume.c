/* E1: actual cached-volume snapshot code, transaction/update state, and sector
 * fixture. No disk image, fake transaction result, or QEMU claim is involved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "persist_volume_fixture.c"

static SfTrustKey local_keys[2];
#define VCHECK(c) do{checks++;if(!(c)){fprintf(stderr,"snapshot volume failure line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static int filled(const void*p,size_t n,uint8_t value){const uint8_t*b=p;for(size_t i=0;i<n;i++)if(b[i]!=value)return 0;return 1;}

static void set_file(unsigned slot,const char*path,const void*data,uint32_t bytes){
 SfTxnFile*f=&volume.files[slot];memset(f,0,sizeof(*f));strcpy(f->path,path);f->bytes=bytes;if(bytes)memcpy(f->data,data,bytes);
}
static void setup(void){
 memset(&volume,0,sizeof(volume));volume.ready=1;volume.generation=9;
 for(unsigned i=0;i<16;i++)volume.volume[i]=(uint8_t)(0x10+i);
 set_file(0,"/z","",0);set_file(1,"/notes/user","snapshot",8);
 const uint8_t alpha[]={1,2,3};set_file(2,"/a",alpha,sizeof(alpha));
 memset(local_keys,0,sizeof(local_keys));
 for(unsigned i=0;i<32;i++){local_keys[0].public_key[i]=(uint8_t)(i+1);local_keys[1].public_key[i]=(uint8_t)(0xc0-i);}
 local_keys[0].usage=SF_TRUST_MODULE;local_keys[1].usage=SF_TRUST_APP;
 SfTrustStore policy={local_keys,2};VCHECK(!sf_trust_init(&sf_trust,&policy));persist_fixture_device(1);
}
static void no_mutation_get(SfPersistSnapshot*out,int expected){
 persist_fixture_snapshot();SfPersistSnapshot before;memset(&before,0xa5,sizeof(before));
 int r=persist_snapshot_get(&before);VCHECK(r==expected);
 if(expected)VCHECK(filled(&before,sizeof(before),0xa5));
 VCHECK(persist_fixture_unchanged());if(out)*out=before;
}
static int hexval(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;return -1;}
static void canonical_and_invariance(const char*expected_hex){
 SfPersistSnapshot a,b;no_mutation_get(&a,0);
 VCHECK(sizeof(a)==104&&a.size==104&&a.version==1&&a.generation==9&&a.notes_bytes==8);
 for(unsigned i=0;i<16;i++)VCHECK(a.volume[i]==(uint8_t)(0x10+i));
 no_mutation_get(&b,0);VCHECK(!memcmp(&a,&b,sizeof(a)));
 /* The physical slot order changes, while the logical cached file set stays the same. */
 SfTxnFile saved[SF_TXN_FILES];memcpy(saved,volume.files,sizeof(saved));
 volume.files[0]=saved[2];volume.files[2]=saved[0];no_mutation_get(&b,0);
 VCHECK(!memcmp(a.files_sha256,b.files_sha256,32));memcpy(volume.files,saved,sizeof(saved));
 if(expected_hex){
  VCHECK(strlen(expected_hex)==64);uint8_t expected[32];
  for(unsigned i=0;i<32;i++){int hi=hexval(expected_hex[2*i]),lo=hexval(expected_hex[2*i+1]);VCHECK(hi>=0&&lo>=0);expected[i]=(uint8_t)((hi<<4)|lo);}
  VCHECK(!memcmp(a.files_sha256,expected,32));
 }
}
static void semantic_changes(void){
 SfPersistSnapshot base,changed;no_mutation_get(&base,0);
 uint8_t old=volume.files[2].data[0];volume.files[2].data[0]^=0x40;no_mutation_get(&changed,0);
 VCHECK(memcmp(base.files_sha256,changed.files_sha256,32));volume.files[2].data[0]=old;
 volume.files[1].bytes=7;no_mutation_get(&changed,0);VCHECK(changed.notes_bytes==7&&memcmp(base.files_sha256,changed.files_sha256,32));volume.files[1].bytes=8;
 uint64_t generation=volume.generation;volume.generation++;
 no_mutation_get(&changed,0);VCHECK(changed.generation==generation+1&&!memcmp(base.files_sha256,changed.files_sha256,32));volume.generation=generation;
 volume.volume[0]^=1;no_mutation_get(&changed,0);VCHECK(memcmp(base.volume,changed.volume,16)&&!memcmp(base.files_sha256,changed.files_sha256,32));volume.volume[0]^=1;
 local_keys[0].usage=SF_TRUST_MODULE|SF_TRUST_APP;no_mutation_get(&changed,0);
 VCHECK(memcmp(base.trust_sha256,changed.trust_sha256,32));local_keys[0].usage=SF_TRUST_MODULE;
}
static void no_mutation_errors(void){
 SfPersistSnapshot out;memset(&out,0xa5,sizeof(out));SfPersistSnapshot before=out;
 volume.ready=0;persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_DEVICE_GONE&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());volume.ready=1;
 persist_fixture_device(0);persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_DEVICE_GONE&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());persist_fixture_device(1);
 SfUpdate trial={0};uint8_t hash[32];memset(hash,0x61,sizeof(hash));VCHECK(!sf_update_stage(&trial,"org.spiritfox.demo",hash)&&!sf_update_begin(&trial));
 uint8_t encoded[SF_UPDATE_BYTES];VCHECK(!sf_update_encode(&trial,encoded));set_file(3,"/boot/current",encoded,sizeof(encoded));
 persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_BUSY&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());
 memset(&volume.files[3],0,sizeof(volume.files[3]));

 SfTxnFile saved[SF_TXN_FILES];memcpy(saved,volume.files,sizeof(saved));
 set_file(3,"/a","duplicate",9);persist_fixture_snapshot();
 VCHECK(persist_snapshot_get(&out)==SF_BAD_FORMAT&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());
 memcpy(volume.files,saved,sizeof(saved));
 memset(volume.files[3].path,'x',sizeof(volume.files[3].path));volume.files[3].bytes=1;
 persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_BAD_FORMAT&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());
 memcpy(volume.files,saved,sizeof(saved));volume.files[2].bytes=SF_TXN_FILE_BYTES+1;
 persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_BAD_FORMAT&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());
 memcpy(volume.files,saved,sizeof(saved));uint64_t generation=volume.generation;volume.generation=0;
 persist_fixture_snapshot();VCHECK(persist_snapshot_get(&out)==SF_BAD_FORMAT&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());volume.generation=generation;
 SfRuntimeTrust saved_trust=sf_trust;sf_trust.policy.count=0;persist_fixture_snapshot();
 VCHECK(persist_snapshot_get(&out)==SF_BAD_FORMAT&&!memcmp(&out,&before,sizeof(out))&&persist_fixture_unchanged());sf_trust=saved_trust;
}
int main(int argc,char**argv){
 setup();canonical_and_invariance(argc>1?argv[1]:0);semantic_changes();no_mutation_errors();
 printf("PERSIST SNAPSHOT VOLUME PASS %u checks; actual cache/update code with memory sector fixture; no hardware or guest claim; file-digest vector %s\n",
        checks,argc>1?"provided":"external-vector pending");return 0;
}
