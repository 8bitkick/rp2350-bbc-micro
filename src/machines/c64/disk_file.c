// Frodo's stdio-facing disk interface backed by a private image in PSRAM.
// Writes survive for this session only; the SD original is never overwritten.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
typedef struct FATFS_FILE_HANDLE { size_t pos; int writable; } FATFS_FILE;
static uint8_t *disk;
static size_t disk_size;
static uint32_t writes;
void c64_disk_set(uint8_t *p,size_t n) {disk=p;disk_size=n;writes=0;}
uint32_t c64_disk_writes(void) {return writes;}
FATFS_FILE *fatfs_fopen(const char *path,const char *mode) {
    if(!disk || !disk_size || strcmp(path,"presto.d64") || strchr(mode,'w') || strchr(mode,'a'))return NULL;
    FATFS_FILE *f=calloc(1,sizeof *f);if(f)f->writable=strchr(mode,'+')!=NULL;return f;
}
int fatfs_fclose(FATFS_FILE *f){free(f);return 0;}
size_t fatfs_fread(void *p,size_t size,size_t n,FATFS_FILE *f){
    if(!f || !size || n>SIZE_MAX/size || f->pos>disk_size)return 0;
    size_t bytes=size*n;if(bytes>disk_size-f->pos)bytes=disk_size-f->pos;
    memcpy(p,disk+f->pos,bytes);f->pos+=bytes;return bytes/size;
}
size_t fatfs_fwrite(const void *p,size_t size,size_t n,FATFS_FILE *f){
    if(!f || !f->writable || !size || n>SIZE_MAX/size || f->pos>disk_size)return 0;
    size_t bytes=size*n;if(bytes>disk_size-f->pos)bytes=disk_size-f->pos;
    memcpy(disk+f->pos,p,bytes);f->pos+=bytes;if(bytes)writes++;return bytes/size;
}
int fatfs_fseek(FATFS_FILE *f,long off,int whence){
    if(!f)return -1;
    int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)f->pos:whence==SEEK_END?(int64_t)disk_size:-1;
    if(base<0 || (off<0 && off < -base) || (off>=0 && (uint64_t)off>disk_size-(size_t)base))return -1;
    f->pos=(size_t)(base+off);return 0;
}
long fatfs_ftell(FATFS_FILE *f){return f?(long)f->pos:-1;}
int fatfs_feof(FATFS_FILE *f){return !f || f->pos>=disk_size;}
int fatfs_getc(FATFS_FILE *f){uint8_t c;return fatfs_fread(&c,1,1,f)==1?c:EOF;}
int fatfs_putc(int c,FATFS_FILE *f){uint8_t b=c;return fatfs_fwrite(&b,1,1,f)==1?b:EOF;}
void fatfs_rewind(FATFS_FILE *f){if(f)f->pos=0;}
