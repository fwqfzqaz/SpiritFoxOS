/* E1: actual transaction/update/cache code over the repository's persistent
 * image copied into memory media. Candidate sector I/O is read-only and local;
 * this is not guest, hardware, or power-loss evidence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "persist_volume_fixture.c"

#define CVCHECK(c) do{checks++;if(!(c)){fprintf(stderr,"candidate volume failure line %d: %s\n",__LINE__,#c);exit(1);}}while(0)

static uint8_t candidate_media[sizeof(media)], valid_candidate_media[sizeof(media)], media_before[sizeof(media)],candidate_before[sizeof(media)];
static SfRuntimeTrust candidate_trust_before;
static unsigned candidate_reads, candidate_writes_before, candidate_flushes_before;
static unsigned candidate_ranges_before, candidate_brackets_before;
static uint64_t highest_lba;
static int bad_lba=-1, read_error;
static uint8_t revoked_id[32];

static int candidate_read(void*context,uint64_t lba,uint8_t out[512]){
 uint8_t*bytes=context;
 candidate_reads++;
 CVCHECK(lba>=SF_TXN_HEADER&&lba<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);
 if(lba>highest_lba)highest_lba=lba;
 if((int)lba==bad_lba)return read_error;
 memcpy(out,bytes+lba*512,512);return 0;
}

static void load_live_image(void){
 FILE*f=fopen("build/persistent.img","rb");
 CVCHECK(f&&fread(media,1,sizeof(media),f)==sizeof(media)&&fgetc(f)==EOF);
 CVCHECK(!fclose(f));
 persist_fixture_device(1);
 CVCHECK(!sf_trust_init(&sf_trust,&sf_trust_store));
 uint64_t notes=UINT64_MAX;CVCHECK(!persist_mount(&notes));
 CVCHECK(notes<=SF_TXN_FILE_BYTES);
}

static void add_live_note_and_revocation(void){
 static const char note[]="candidate-volume-note";
 CVCHECK(persist_notes_write(0,note,sizeof(note)-1)==(long)sizeof(note)-1);
 static const uint8_t seed[]="persist-candidate-runtime-revocation";
 sf_sha256(revoked_id,seed,sizeof(seed)-1);
 CVCHECK(!sf_trust_revoke(&sf_trust,revoked_id));
 const char*paths[]={"/boot/revoked"};const void*data[]={revoked_id};
 uint32_t lengths[]={sizeof(revoked_id)};
 CVCHECK(!commit(paths,data,lengths,1));
}

static void set_update_phase(unsigned phase){
 SfUpdate state={0};uint8_t encoded[SF_UPDATE_BYTES];
 if(phase!=SF_UPDATE_STABLE){
  uint8_t candidate_hash[32];memset(candidate_hash,0x6d,sizeof(candidate_hash));
  CVCHECK(!sf_update_stage(&state,"org.spiritfox.demo",candidate_hash));
  if(phase==SF_UPDATE_TRYING)CVCHECK(!sf_update_begin(&state));
 }
 CVCHECK(state.phase==phase&&!sf_update_encode(&state,encoded));
 const char*paths[]={"/boot/current"};const void*data[]={encoded};
 uint32_t lengths[]={sizeof(encoded)};CVCHECK(!commit(paths,data,lengths,1));
}

static SfPersistSnapshot prepare_image(unsigned phase){
 load_live_image();add_live_note_and_revocation();
 set_update_phase(phase);
 SfPersistSnapshot snapshot;memset(&snapshot,0xa5,sizeof(snapshot));
 int r=persist_snapshot_get(&snapshot);
 CVCHECK(phase==SF_UPDATE_TRYING?r==SF_BUSY:r==0);
 if(phase==SF_UPDATE_TRYING){
  SfUpdate state;CVCHECK(!state_read(&state)&&state.phase==SF_UPDATE_TRYING);
  /* The expected snapshot is the last safe PENDING observation. */
  CVCHECK(!sf_update_finish(&state,0));uint8_t encoded[SF_UPDATE_BYTES];
  CVCHECK(!sf_update_stage(&state,"org.spiritfox.demo",revoked_id));
  CVCHECK(!sf_update_encode(&state,encoded));
  const char*paths[]={"/boot/current"};const void*data[]={encoded};
  uint32_t lengths[]={sizeof(encoded)};CVCHECK(!commit(paths,data,lengths,1));
  CVCHECK(!persist_snapshot_get(&snapshot));
  CVCHECK(!state_read(&state)&&!sf_update_begin(&state)&&!state_commit(&state));
 }else if(phase==SF_UPDATE_PENDING){
  CVCHECK(snapshot.generation==volume.generation);
 }else{
  CVCHECK(snapshot.generation==volume.generation);
 }
 CVCHECK(snapshot.size==sizeof(snapshot)&&snapshot.version==1&&snapshot.notes_bytes==sizeof("candidate-volume-note")-1);
 memcpy(candidate_media,media,sizeof(media));
 memcpy(valid_candidate_media,media,sizeof(media));
 return snapshot;
}

static void candidate_fresh(void){
 memset(&volume,0,sizeof(volume));
 memset(scratch,0,sizeof(scratch));memset(stage,0,sizeof(stage));memset(export,0,sizeof(export));
 stage_bytes=0;export_bytes=-1;export_preview=-1;
 CVCHECK(!sf_trust_init(&sf_trust,&sf_trust_store));
}

static void before_call(void){
 memcpy(media_before,media,sizeof(media));
 memcpy(candidate_before,candidate_media,sizeof(candidate_media));candidate_trust_before=sf_trust;
 candidate_reads=0;highest_lba=0;bad_lba=-1;read_error=SF_DEVICE_GONE;
 candidate_writes_before=writes;candidate_flushes_before=flushes;
 candidate_ranges_before=ranges;candidate_brackets_before=brackets;
 persist_fixture_device(1);
}

static void check_io_and_quarantine(int expect_success){
 CVCHECK(!memcmp(media,media_before,sizeof(media))&&!memcmp(candidate_media,candidate_before,sizeof(candidate_media))&&writes==candidate_writes_before&&
         flushes==candidate_flushes_before&&ranges==candidate_ranges_before&&brackets==candidate_brackets_before);
 if(candidate_reads){
  CVCHECK(candidate_reads<=3+2*SF_TXN_BANK_SECTORS);
  CVCHECK(highest_lba<SF_TXN_BANK+2*SF_TXN_BANK_SECTORS);
 }
 CVCHECK(volume.read==0&&volume.context==0);
 if(!expect_success)CVCHECK(!volume.ready&&!memcmp(&sf_trust,&candidate_trust_before,sizeof(sf_trust)));
}

static void success_case(unsigned phase){
 SfPersistSnapshot expected=prepare_image(phase),out;candidate_fresh();before_call();
 memset(&out,0xa5,sizeof(out));
 CVCHECK(!persist_candidate_verify_volume(&expected,candidate_read,candidate_media,&out));
 CVCHECK(!memcmp(&out,&expected,sizeof(out))&&volume.ready);
 CVCHECK(sf_trust.revoked==1&&!memcmp(sf_trust.revoked_ids[0],revoked_id,32));
 check_io_and_quarantine(1);
 uint8_t sector[512]={0};
 CVCHECK(volume.write&&volume.write(volume.context,2048,sector)==SF_READ_ONLY);
 CVCHECK(volume.flush&&volume.flush(volume.context)==SF_READ_ONLY);
 CVCHECK(volume.read==0&&volume.context==0&&writes==candidate_writes_before&&flushes==candidate_flushes_before);
}

static void mismatch_cases(void){
 SfPersistSnapshot expected=prepare_image(SF_UPDATE_STABLE);
 for(unsigned field=0;field<6;field++){
  SfPersistSnapshot bad=expected,out,before;candidate_fresh();
  switch(field){
   case 0:bad.size--;break;case 1:bad.generation++;break;case 2:bad.volume[0]^=1;break;
   case 3:bad.files_sha256[0]^=1;break;case 4:bad.notes_bytes++;break;default:bad.trust_sha256[0]^=1;break;
  }
  memset(&out,0xa5,sizeof(out));before=out;before_call();
  int r=persist_candidate_verify_volume(&bad,candidate_read,candidate_media,&out);
  CVCHECK(r<0&&!memcmp(&out,&before,sizeof(out)));
  check_io_and_quarantine(0);
 }
}

static void malformed_volume_cases(void){
 SfPersistSnapshot expected,out,before;
 for(unsigned kind=0;kind<4;kind++){
  expected=prepare_image(SF_UPDATE_STABLE);unsigned active_bank=volume.bank;
  if(kind==2){
   uint8_t duplicate[64];memcpy(duplicate,revoked_id,32);memcpy(duplicate+32,revoked_id,32);
   const char*paths[]={"/boot/revoked"};const void*data[]={duplicate};uint32_t lengths[]={sizeof(duplicate)};
   CVCHECK(!commit(paths,data,lengths,1));
  }
  if(kind==3){
   uint8_t partial[31];memset(partial,0x92,sizeof(partial));
   const char*paths[]={"/boot/revoked"};const void*data[]={partial};uint32_t lengths[]={sizeof(partial)};
   CVCHECK(!commit(paths,data,lengths,1));
  }
  memcpy(candidate_media,media,sizeof(media));
  if(kind==0)candidate_media[SF_TXN_HEADER*512]^=0x80;
  if(kind==1)candidate_media[(SF_TXN_BANK+active_bank*SF_TXN_BANK_SECTORS)*512]^=0x80;
  candidate_fresh();
  memset(&out,0xa5,sizeof(out));before=out;before_call();
  if(kind==2||kind==3)CVCHECK(!sf_trust_init(&sf_trust,&sf_trust_store));
  int r=persist_candidate_verify_volume(&expected,candidate_read,candidate_media,&out);
  CVCHECK(r<0&&!memcmp(&out,&before,sizeof(out)));
  check_io_and_quarantine(0);
 }
 /* A read failure on the authenticated volume header aborts without output. */
 memcpy(candidate_media,valid_candidate_media,sizeof(candidate_media));candidate_fresh();
 memset(&out,0xa5,sizeof(out));before=out;before_call();bad_lba=SF_TXN_HEADER;read_error=SF_DEVICE_GONE;
 int r=persist_candidate_verify_volume(&expected,candidate_read,candidate_media,&out);
 CVCHECK(r==SF_DEVICE_GONE&&!memcmp(&out,&before,sizeof(out)));check_io_and_quarantine(0);
}

static void precondition_cases(void){
 SfPersistSnapshot expected=prepare_image(SF_UPDATE_STABLE),out,before;
 for(unsigned kind=0;kind<3;kind++){
  candidate_fresh();if(kind==0)volume.ready=1;else if(kind==1)stage_bytes=1;else export_bytes=0;
  SfTxn volume_before=volume;
  memset(&out,0xa5,sizeof(out));before=out;before_call();
  CVCHECK(persist_candidate_verify_volume(&expected,candidate_read,candidate_media,&out)==SF_BUSY);
  CVCHECK(!memcmp(&out,&before,sizeof(out))&&!candidate_reads&&!memcmp(&volume,&volume_before,sizeof(volume)));
  CVCHECK(!memcmp(media,media_before,sizeof(media))&&writes==candidate_writes_before&&flushes==candidate_flushes_before);
 }
}

int main(void){
 success_case(SF_UPDATE_STABLE);success_case(SF_UPDATE_PENDING);
 SfPersistSnapshot pending=prepare_image(SF_UPDATE_PENDING),out,before;
 /* A real transaction transition to TRYING is rejected read-only. */
 CVCHECK(persist_begin()==1); /* PENDING must not be confused with a fabricated cache result. */
 memcpy(candidate_media,media,sizeof(media));
 candidate_fresh();memset(&out,0xa5,sizeof(out));before=out;before_call();
 int r=persist_candidate_verify_volume(&pending,candidate_read,candidate_media,&out);
 CVCHECK(r==SF_BUSY&&!memcmp(&out,&before,sizeof(out)));check_io_and_quarantine(0);
 mismatch_cases();malformed_volume_cases();precondition_cases();
 printf("PERSIST CANDIDATE VOLUME PASS %u checks; actual transaction/cache, memory-sector E1; no guest or hardware claim\n",checks);
 return 0;
}
