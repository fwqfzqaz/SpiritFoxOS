#include "persist_candidate.h"
#include "launch_policy.h"
#include "native_image.h"

static int address_range(uintptr_t start,uint64_t bytes,uintptr_t *end){
 if(bytes>(uint64_t)(UINTPTR_MAX-start))return 0;
 *end=start+(uintptr_t)bytes;return 1;
}
static int ranges_overlap(uintptr_t a,uintptr_t ae,uintptr_t b,uintptr_t be){
 return a<be&&b<ae;
}

int sf_persist_candidate_prepare(const SfRuntimeTrust *trust,const void *data,
                                 uint64_t bytes,void *staging,size_t capacity,
                                 SfPersistCandidateImage *out){
 if(!trust||!data||!staging||!out)return SF_INVALID;
 uintptr_t trust_start=(uintptr_t)trust,data_start=(uintptr_t)data;
 uintptr_t stage_start=(uintptr_t)staging,out_start=(uintptr_t)out;
 uintptr_t trust_end,data_end,stage_end,out_end;
 if(!address_range(trust_start,sizeof(*trust),&trust_end))return SF_INVALID;
 if(!address_range(data_start,bytes,&data_end)||
    !address_range(stage_start,(uint64_t)capacity,&stage_end)||
    !address_range(out_start,sizeof(*out),&out_end))return SF_INVALID;
 /* Aliased spans are rejected before touching either caller-owned buffer. */
 if(ranges_overlap(trust_start,trust_end,data_start,data_end)||
    ranges_overlap(trust_start,trust_end,stage_start,stage_end)||
    ranges_overlap(trust_start,trust_end,out_start,out_end)||
    ranges_overlap(data_start,data_end,stage_start,stage_end)||
    ranges_overlap(data_start,data_end,out_start,out_end)||
    ranges_overlap(stage_start,stage_end,out_start,out_end))return SF_INVALID;
 /* The immutable policy is indirect input too: scratch/output may not alias
  * its key table even when the SfRuntimeTrust header itself is disjoint. */
 uintptr_t keys_start=(uintptr_t)trust->policy.keys,keys_end;
 if(!trust->policy.keys||!trust->policy.count||trust->policy.count>SF_TRUST_MAX||
    !address_range(keys_start,(uint64_t)trust->policy.count*sizeof(SfTrustKey),&keys_end)||
    ranges_overlap(keys_start,keys_end,data_start,data_end)||
    ranges_overlap(keys_start,keys_end,stage_start,stage_end)||
    ranges_overlap(keys_start,keys_end,out_start,out_end))return SF_INVALID;

 /* For disjoint valid spans, every subsequent error leaves a zero result. */
 memset(out,0,sizeof(*out));
 if(!bytes||bytes>SF_PACKAGE_MAX||!capacity||capacity>SF_IMAGE_MAX)return SF_INVALID;

 SfPackage package;
 int r=sf_trust_open(trust,&package,data,bytes);if(r)return r;
 if(package.kind!=1)return SF_DENIED;

 char module_id[64],category[32];
 if(sf_manifest_text(&package,"module_id",module_id,sizeof(module_id))||
    strcmp(module_id,"org.spiritfox.persist")||
    sf_manifest_text(&package,"category",category,sizeof(category))||
    strcmp(category,"service"))return SF_DENIED;

 if(package.native_profile!=SF_NATIVE_STATIC||
    package.native_role!=SF_NATIVE_ROLE_PROCESS||
    package.import_count||package.export_count)return SF_DEPENDENCY;
 r=sf_launch_admit(&package,1,0,0);if(r)return r;

 SfPersistCandidateImage candidate;memset(&candidate,0,sizeof(candidate));
 candidate.plan.size=sizeof(candidate.plan);
 r=sf_native_image(&package,SF_BASE,staging,capacity,&candidate.plan);if(r)return r;

 /* Keep authority fields manager-owned even if the planner grows later. */
 candidate.plan.grant_manager=0;candidate.plan.grant_service=0;
 candidate.plan.service_rights=0;candidate.plan.grant_bundle=0;
 candidate.plan.grant_device=0;candidate.plan.device_rights=0;
 memset(&candidate.plan.arguments,0,sizeof(candidate.plan.arguments));
 candidate.bytes=bytes;
 sf_sha256(candidate.source_sha256,data,(size_t)bytes);
 memcpy(candidate.key_id,package.key_id,sizeof(candidate.key_id));
 memcpy(out,&candidate,sizeof(candidate));
 return SF_OK;
}
