#ifndef SF_STORAGE_REPLY_UNAVAILABLE_HOST_H
#define SF_STORAGE_REPLY_UNAVAILABLE_HOST_H
/* These older client fixtures do not model Root's shared block-reply owner.
 * Resolve the new private boundary for linking, but fail on any invocation.
 * The real adapter is exercised by test_persist_reply_owner.c instead. */
int sf_storage_reply_status(SfStorageReplyOwner*out){(void)out;CHECK(0);return SF_DENIED;}
int sf_storage_reply_access(void){CHECK(0);return SF_DENIED;}
int sf_storage_reply_claim_trusted(const SfStorageReplySeal*s,uint64_t*out){
 (void)s;(void)out;CHECK(0);return SF_DENIED;
}
int sf_storage_reply_return_trusted(const SfStorageReplySeal*s,uint64_t serial){
 (void)s;(void)serial;CHECK(0);return SF_DENIED;
}
int sf_storage_reply_taint_trusted(const SfStorageReplySeal*s,uint64_t serial,int error){
 (void)s;(void)serial;(void)error;CHECK(0);return SF_DENIED;
}
#endif
