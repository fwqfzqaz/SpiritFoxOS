#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "linux_image.h"
static uint8_t *load_file(const char *path,size_t *bytes){
 FILE *f=fopen(path,"rb");
 if(!f)return 0;
 if(fseek(f,0,SEEK_END)){fclose(f);return 0;}
 long n=ftell(f);
 if(n<0||fseek(f,0,SEEK_SET)){fclose(f);return 0;}
 uint8_t *data=(uint8_t*)malloc((size_t)n? (size_t)n:1);
 if(!data){fclose(f);return 0;}
 if(n&&fread(data,1,(size_t)n,f)!=(size_t)n){free(data);fclose(f);return 0;}
 fclose(f);*bytes=(size_t)n;return data;
}
int main(int argc,char **argv){
 if(argc<2||argc>3){
  fprintf(stderr,"usage: linux_elf_check <elf> [image-base]\n");
  return 2;
 }
 size_t bytes=0;uint8_t*data=load_file(argv[1],&bytes);
 if(!data){fprintf(stderr,"read failed: %s\n",argv[1]);return 2;}
 SfLinuxInfo info;
 int r=sf_linux_elf_validate(data,bytes,&info);
 if(r){
  printf("{\"status\":\"rejected\",\"code\":%d}\n",r);
  free(data);return 2;
 }
 if(argc==2){
  printf("{\"status\":\"valid\",\"entry_rva\":%llu,\"image_bytes\":%llu,"
         "\"phdr_rva\":%llu,\"phnum\":%u,\"relocations\":%u}\n",
         (unsigned long long)info.entry_rva,(unsigned long long)info.image_bytes,
         (unsigned long long)info.phdr_rva,info.phnum,info.relocations);
  free(data);return 0;
 }
 uint64_t base=strtoull(argv[2],0,0);
 uint8_t*staging=(uint8_t*)malloc((size_t)info.image_bytes? (size_t)info.image_bytes:1);
 SfLoadPlan plan;
 r=staging? sf_linux_elf_load(data,bytes,base,staging,(size_t)info.image_bytes,&plan,&info,0):SF_NO_SPACE;
 free(data);
 if(r){
  printf("{\"status\":\"rejected\",\"code\":%d}\n",r);
  free(staging);return 2;
 }
 printf("{\"status\":\"loaded\",\"base\":%llu,\"entry\":%llu,\"count\":%u,"
        "\"syscalls\":%u,\"segments\":[",
        (unsigned long long)base,(unsigned long long)(base+plan.entry),plan.count,info.syscalls);
 for(unsigned i=0;i<plan.count;i++)
  printf("%s{\"rva\":%llu,\"bytes\":%llu,\"flags\":%u}",i?",":"",
         (unsigned long long)plan.segments[i].rva,
         (unsigned long long)plan.segments[i].bytes,plan.segments[i].flags);
 printf("]}\n");
 free(staging);return 0;
}
