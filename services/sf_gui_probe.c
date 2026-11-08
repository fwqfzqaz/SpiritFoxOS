/* sf-gui-probe：演示应用注册。启动后把自身登记到合成器目录，
 * 之后常驻让出 CPU；持有 WM service 端点的发送权即视为可注册。 */
#include "libsf.h"
static int publish(uint32_t endpoint,const char*id,const char*title,uint64_t request){
 SfMessage m;sf_message(&m,OP_WM_REGISTER,request);
 uint32_t il=0;while(id[il])il++;
 uint32_t tl=0;while(title[tl])tl++;
 if(il+1u+tl+1u>sizeof(m.data))return SF_INVALID;
 for(uint32_t i=0;i<il;i++)m.data[i]=(uint8_t)id[i];
 m.data[il]=0;
 for(uint32_t i=0;i<tl;i++)m.data[il+1u+i]=(uint8_t)title[i];
 m.data[il+1u+tl]=0;
 m.length=il+1u+tl+1u;
 return (int)sf_send(endpoint,&m);
}
long sf_main(const SfBootArgs*a,size_t size){
 if(!a||size!=sizeof(*a)||a->abi_major!=1)return SF_ABI_MISMATCH;
 struct {const char*id,*title;} apps[]={
  {"org.spiritfox.files","FILES"},
  {"org.spiritfox.notes","NOTES"},
  {"org.spiritfox.settings","SETTINGS"}
 };
 for(unsigned i=0;i<sizeof(apps)/sizeof(apps[0]);i++){
  int r=publish(a->service,apps[i].id,apps[i].title,i+1);
  if(r){
   sf_log("GUI-PROBE: register failed ");
   sf_log(apps[i].title);sf_log("\n");
   return 1;
  }
 }
 for(;;)sf_yield();
}
