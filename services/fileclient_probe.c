#include "libsf.h"
#include "arguments.h"
#include "sf_file.h"
long sf_main(const SfBootArgs*a,size_t bytes){
 if(sf_boot_arguments_validate(a,bytes))return 1;
 SfFileClient c;if(sf_file_client_init(&c,a->manager,a->service,3000))return 2;
 int64_t console=sf_file_console(&c);if(console<0)return 3;
 const char text[]="FILE SDK CONSOLE\n";if(sf_file_write(&c,console,text,sizeof(text)-1)!=(int64_t)(sizeof(text)-1))return 4;
 int64_t root=sf_file_root(&c);
 if(root==SF_DENIED){if(sf_file_close(&c,console)||c.error)return 5;sf_log("FILE SDK NO ROOT\n");return 0;}
 if(root<0)return 6;
 SfFileStat info;if(sf_file_stat(&c,root,&info)||info.type!=SF_TYPE_DIRECTORY||info.mode!=(SF_OPEN_READ|SF_OPEN_DIRECTORY))return 7;
 if(sf_file_openat(&c,root,"../escape",SF_OPEN_READ)!=SF_INVALID)return 8;
 if(sf_file_close(&c,(1ull<<32)|(uint64_t)root)!=SF_STALE_HANDLE||sf_file_stat(&c,root,&info))return 9;
 int64_t root_copy=sf_file_dup(&c,root);if(root_copy<=0||root_copy==root||sf_file_stat(&c,root_copy,&info)||info.mode!=(SF_OPEN_READ|SF_OPEN_DIRECTORY))return 24;
 const char*path=a->argc>1?sf_argument(a,bytes,1):"notes";if(!path)return 10;
 int64_t file=sf_file_openat(&c,root,path,SF_OPEN_READ);if(file<0)return 11;
 if(sf_file_stat(&c,file,&info)||(info.type!=SF_TYPE_FILE&&info.type!=SF_TYPE_DISK)||info.mode!=SF_OPEN_READ||!info.size||info.offset)return 12;
 int64_t file_copy=sf_file_dup(&c,file);if(file_copy<=0||file_copy==file||sf_file_stat(&c,file_copy,&info)||info.mode!=SF_OPEN_READ)return 25;
 uint8_t data[256],second[256];int64_t n=sf_file_read(&c,file,data,sizeof(data));if(n<=0||n>256)return 13;
 if(sf_file_stat(&c,file,&info)||info.offset!=(uint64_t)n||sf_file_poll(&c,file)!=1)return 14;
 if(sf_file_stat(&c,file_copy,&info)||info.offset!=(uint64_t)n||sf_file_write(&c,file_copy,data,1)!=SF_DENIED||sf_file_truncate(&c,file_copy,0)!=SF_DENIED)return 26;
 if(sf_file_pread(&c,file_copy,second,sizeof(second),0)!=n||memcmp(data,second,n)||sf_file_stat(&c,file,&info)||info.offset!=(uint64_t)n)return 29;
 if(sf_file_pread(&c,file_copy,second,1,UINT64_MAX)||sf_file_stat(&c,file_copy,&info)||info.offset!=(uint64_t)n||sf_file_pread(&c,root,second,1,0)!=SF_INVALID)return 30;
 if(sf_file_pread(&c,file_copy,second,1,1)!=(n>1?1:0)||(n>1&&second[0]!=data[1])||sf_file_stat(&c,file,&info)||info.offset!=(uint64_t)n)return 31;
 if(sf_file_read(&c,file,0,0)||sf_file_write(&c,file,0,0)!=SF_DENIED)return 15;
 if(sf_file_seek(&c,file_copy,0)||sf_file_stat(&c,file,&info)||info.offset||sf_file_read(&c,file_copy,second,sizeof(second))!=n||memcmp(data,second,n))return 16;
 if(sf_file_enumerate(&c,root,(char*)second,sizeof(second))<=0)return 17;
 if(sf_file_openat(&c,root,path,SF_OPEN_WRITE)!=SF_DENIED||sf_file_truncate(&c,file,0)!=SF_DENIED)return 18;
 if(sf_file_unlink(&c,root,path)!=SF_DENIED||sf_file_rmdir(&c,root,path)!=SF_DENIED)return 19;
 if(sf_file_close(&c,root)||sf_file_root(&c)!=SF_DENIED||sf_file_stat(&c,file,&info))return 20;
 int64_t child=sf_file_openat(&c,root_copy,path,SF_OPEN_READ);if(child<=0||sf_file_close(&c,child)||sf_file_close(&c,root_copy)||sf_file_root(&c)!=SF_DENIED)return 27;
 if(sf_file_close(&c,file))return 21;memset(&info,0xa5,sizeof(info));SfFileStat prior=info;
 if(sf_file_stat(&c,file,&info)!=SF_STALE_HANDLE||memcmp(&info,&prior,sizeof(info)))return 22;
 uint8_t one;if(sf_file_stat(&c,file_copy,&info)||sf_file_seek(&c,file_copy,0)||sf_file_read(&c,file_copy,&one,1)!=1||one!=data[0]||sf_file_close(&c,file_copy))return 28;
 if(sf_file_close(&c,console)||c.error)return 23;
 sf_log("FILE PREAD APP PASSED\n");sf_log("FILE DUP APP PASSED\n");sf_log("FILE SDK APP PASSED\n");return 0;
}
