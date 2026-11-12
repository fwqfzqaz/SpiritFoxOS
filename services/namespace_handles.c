#include "namespace_handles.h"
_Static_assert(SF_NS_HANDLE_MAX<=255,"client handle slot encoding");
static int remote_valid(uint64_t h){
 if(h>UINT32_MAX)return 0;
 unsigned slot=(unsigned)(h&255);uint64_t generation=h>>8;
 return slot&&slot<=SF_NS_HANDLE_MAX&&generation&&generation<=SF_NSC_GENERATION_MAX;
}
void sf_nsh_init(SfNsHandleMap*m){memset(m,0,sizeof(*m));m->epoch=1;}
int sf_nsh_prepare(const SfNsHandleMap*m,uint32_t owner,SfNsHandleReservation*out){
 if(!m||!m->epoch||!owner||!out)return SF_INVALID;
 if(m->exhausted)return SF_NO_SPACE;
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)if(!m->handles[i].remote&&m->handles[i].generation<SF_NSC_GENERATION_MAX){
  *out=(SfNsHandleReservation){i+1,m->handles[i].generation+1,owner,m->epoch};return 0;
 }
 return SF_NO_SPACE;
}
int sf_nsh_commit(SfNsHandleMap*m,const SfNsHandleReservation*r,uint64_t remote){
 if(!m||!r||!r->slot||r->slot>SF_NS_HANDLE_MAX||!r->generation||r->generation>SF_NSC_GENERATION_MAX||!r->owner||!r->epoch)return SF_INVALID;
 if(m->exhausted||r->epoch!=m->epoch)return SF_STALE_HANDLE;
 SfNsClientHandle*h=&m->handles[r->slot-1];
 if(h->remote||h->generation+1!=r->generation)return SF_STALE_HANDLE;
 if(!remote_valid(remote))return SF_BAD_FORMAT;
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)if(m->handles[i].remote==(uint32_t)remote)return SF_BAD_FORMAT;
 *h=(SfNsClientHandle){r->owner,r->generation,(uint32_t)remote};
 return(int)((r->generation<<8)|r->slot);
}
int sf_nsh_lookup(const SfNsHandleMap*m,uint32_t owner,uint64_t local,uint32_t*out){
 if(!m||!out)return SF_INVALID;
 if(!owner||local>UINT32_MAX)return SF_STALE_HANDLE;
 unsigned slot=(unsigned)(local&255);uint64_t generation=local>>8;
 if(!slot||slot>SF_NS_HANDLE_MAX||!generation||generation>SF_NSC_GENERATION_MAX)return SF_STALE_HANDLE;
 const SfNsClientHandle*h=&m->handles[slot-1];
 if(h->owner!=owner||h->generation!=generation||!h->remote)return SF_STALE_HANDLE;
 *out=h->remote;return 0;
}
int sf_nsh_forget(SfNsHandleMap*m,uint32_t owner,uint64_t local){
 uint32_t remote;int r=sf_nsh_lookup(m,owner,local,&remote);if(r)return r;
 SfNsClientHandle*h=&m->handles[(local&255)-1];h->owner=0;h->remote=0;return 0;
}
int sf_nsh_handoff(SfNsHandleMap*m,uint32_t owner,uint64_t local,uint32_t next){
 if(!owner||!next)return SF_INVALID;
 uint32_t remote;int r=sf_nsh_lookup(m,owner,local,&remote);if(r)return r;
 m->handles[(local&255)-1].owner=next;return 0;
}
void sf_nsh_reap(SfNsHandleMap*m,uint32_t owner){
 if(!m||!owner)return;
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++)if(m->handles[i].owner==owner){m->handles[i].owner=0;m->handles[i].remote=0;}
}
void sf_nsh_invalidate(SfNsHandleMap*m){
 if(!m)return;
 for(unsigned i=0;i<SF_NS_HANDLE_MAX;i++){m->handles[i].owner=0;m->handles[i].remote=0;}
 if(m->epoch==UINT64_MAX)m->exhausted=1;else m->epoch++;
}
