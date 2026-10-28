#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/machines/c64/disk_file.c"
int main(void){
 uint8_t storage[819202];memset(storage,0xa5,sizeof storage);c64_disk_set(storage+1,819200);
 assert(!fatfs_fopen("other.d64","rb"));assert(!fatfs_fopen("presto.d64","wb"));
 FATFS_FILE *f=fatfs_fopen("presto.d64","r+b");assert(f);
 assert(fatfs_fseek(f,-1,SEEK_SET)==-1);assert(fatfs_fseek(f,819201,SEEK_SET)==-1);
 assert(!fatfs_fseek(f,-1,SEEK_END));uint8_t bytes[4]={1,2,3,4};
 assert(fatfs_fwrite(bytes,1,4,f)==1);assert(storage[819200]==1);assert(storage[819201]==0xa5);assert(storage[0]==0xa5);
 assert(fatfs_fwrite(bytes,SIZE_MAX,2,f)==0);assert(fatfs_fread(bytes,SIZE_MAX,2,f)==0);
 assert(fatfs_fseek(f,LONG_MAX,SEEK_CUR)==-1);assert(fatfs_fseek(f,LONG_MIN,SEEK_CUR)==-1);
 assert(fatfs_feof(f));assert(c64_disk_writes()==1);fatfs_fclose(f);
 f=fatfs_fopen("presto.d64","rb");assert(fatfs_fwrite(bytes,1,1,f)==0);assert(fatfs_fread(bytes,1,4,f)==4);assert(bytes[0]==0xa5);fatfs_fclose(f);
 c64_disk_set(NULL,0);assert(!fatfs_fopen("presto.d64","rb"));puts("virtual disk bounds: passed");
}
