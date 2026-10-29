#include "board.h"
#include "psram_allocator.h"
#include <new>
#include <cstdlib>
#include <cstring>
#include "pico/stdlib.h"
// PSRAM 0..1 MiB: transfer staging; 1..2 MiB: mounted disk; remainder: C++ heap.
struct alignas(8) Block { size_t size; Block *next; bool free; };
static Block *first;
extern "C" void c64_memory_init(void) {
    if (board_psram_size() < 4*1024*1024) panic("C64 requires PSRAM");
    first = (Block *)(board_psram() + 2*1024*1024);
    first->size = board_psram_size() - 2*1024*1024 - sizeof(Block);
    first->next = nullptr; first->free = true;
}
extern "C" void *psram_malloc(size_t n) {
    if (!first) return malloc(n);
    if (n > 6*1024*1024) return nullptr;
    n = (n + 7) & ~size_t(7); if (!n) n=8;
    for (Block *b=first;b;b=b->next) if(b->free && b->size>=n) {
        if(b->size >= n+sizeof(Block)+8) {
            Block *t=(Block *)((char *)(b+1)+n);
            t->size=b->size-n-sizeof(Block);t->next=b->next;t->free=true;
            b->next=t;b->size=n;
        }
        b->free=false; return b+1;
    }
    return nullptr;
}
extern "C" void psram_free(void *p) {
    if(!p)return;
    if(!first || (uintptr_t)p < (uintptr_t)first || (uintptr_t)p >= (uintptr_t)board_psram()+board_psram_size()){free(p);return;}
    ((Block *)p-1)->free=true;
    for(Block *b=first;b && b->next;) {
        if(b->free && b->next->free) {b->size+=sizeof(Block)+b->next->size;b->next=b->next->next;}
        else b=b->next;
    }
}
extern "C" void *psram_realloc(void *p,size_t n) {
    if(!p)return psram_malloc(n);
    if(!n){psram_free(p);return nullptr;}
    if(!first || (uintptr_t)p < (uintptr_t)first || (uintptr_t)p >= (uintptr_t)board_psram()+board_psram_size())return realloc(p,n);
    Block *b=(Block *)p-1;
    if(n<=b->size)return p;
    void *q=psram_malloc(n);if(q){memcpy(q,p,b->size);psram_free(p);}return q;
}
void *operator new(size_t n) {void *p=psram_malloc(n);if(!p)panic("C64 PSRAM exhausted");return p;}
void *operator new[](size_t n) {return ::operator new(n);}
void operator delete(void *p) noexcept {psram_free(p);}
void operator delete[](void *p) noexcept {psram_free(p);}
void operator delete(void *p,size_t) noexcept {psram_free(p);}
void operator delete[](void *p,size_t) noexcept {psram_free(p);}

extern "C" uint32_t c64_memory_used(void){uint32_t n=2*1024*1024;for(Block *b=first;b;b=b->next)n+=sizeof(Block)+(b->free?0:b->size);return n;}
