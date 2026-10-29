#include <stdint.h>
typedef unsigned UINT;
typedef unsigned FSIZE_t;
typedef struct {unsigned size;} FIL;
#define FR_OK 0
#define FA_READ 1
static inline int f_read(FIL *f,void *p,UINT n,UINT *got){*got=0;return 1;}
static inline int f_open(FIL *f,const char *s,int m){return 1;}
static inline int f_close(FIL *f){return 0;}
static inline FSIZE_t f_size(FIL *f){return f->size;}
