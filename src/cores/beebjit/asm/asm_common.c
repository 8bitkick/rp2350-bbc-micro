#if BEEBJIT_ASM_THUMB2
#include "thumb2/asm_common_thumb2.c"
#elif defined(__x86_64__)
#include "x64/asm_common_x64.c"
#elif defined(__aarch64__)
#include "arm64/asm_common_arm64.c"
#else
#include "null/asm_common_null.c"
#endif
