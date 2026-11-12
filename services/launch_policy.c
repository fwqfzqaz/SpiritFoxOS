#include "launch_policy.h"
#define LIBRARY_ID "org.spiritfox.posix"
int sf_launch_admit(const SfPackage *p,uint16_t expected_kind,int directory,const SfArguments *args){
 if(!p)return SF_INVALID;
 if(p->kind!=expected_kind)return SF_DENIED;
 if(args&&(args->count||args->env_count)&&p->kind!=2)return SF_DENIED;
 if(directory){int r=sf_manifest_capability(p,"namespace.read");if(r<0)return r;if(p->kind!=2||r!=1)return SF_DENIED;}
 if(p->kind==2){char id[96];if(sf_manifest_text(p,"runtime_id",id,sizeof(id))||strcmp(id,"org.spiritfox.native"))return SF_DEPENDENCY;}
 return 0;
}
int sf_launch_library_request(const SfPackage *p){
 if(!p)return SF_INVALID;char id[96];
 return sf_native_library(p,id,sizeof(id))||strcmp(id,LIBRARY_ID)?SF_DEPENDENCY:0;
}
int sf_launch_library_admit(const SfPackage *p){
 if(!p)return SF_INVALID;char id[96];
 return p->kind!=1||p->native_profile!=SF_NATIVE_RELATIVE||p->native_role!=SF_NATIVE_ROLE_LIBRARY||!p->export_count||sf_manifest_text(p,"module_id",id,sizeof(id))||strcmp(id,LIBRARY_ID)?SF_DEPENDENCY:0;
}
