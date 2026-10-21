#pragma once
#include "ee_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EEModuleInfo {
    const char* name;
    uint32_t base;
    uint32_t code_end;
    uint32_t data_end;
    uint32_t bss_end;
    const EEFuncEntry* funcs;
    const unsigned* nfuncs;
} EEModuleInfo;

typedef struct EEOverlaySource {
    const char* name;
    const char* file;    /* ISO path */
    uint32_t offset;     /* byte offset of the code image inside the file */
    uint32_t size;
} EEOverlaySource;

extern const EEModuleInfo g_ee_modules[];
extern const EEOverlaySource g_ee_overlay_sources[];
extern const unsigned g_ee_module_count;

#ifdef __cplusplus
}
#endif
