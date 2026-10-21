// Game-specific IOP module GS_IOP.IRX (sound driver / streaming): recompiled code, loaded here.
#include "gs_iop.h"
#include "iop.h"
#include "iop_kernel.h"
#include "modules.h"
#include "../memory.h"

extern "C" {
extern const IOPFuncEntry gsiop_functable[];
extern const unsigned gsiop_functable_count;
extern const uint32_t gsiop_base, gsiop_entry, gsiop_gp, gsiop_memsz, gsiop_filesz;
extern const uint8_t gsiop_image[];
}

namespace iop::gs_iop {

void register_servers() {}

void start(const std::string& args) {
    log_write(LOG_IOP, "GS_IOP: loading recompiled module at %08x (size %x)", gsiop_base, gsiop_memsz);
    memset(mem::iop_ptr(gsiop_base), 0, gsiop_memsz);
    memcpy(mem::iop_ptr(gsiop_base), gsiop_image, gsiop_filesz);
    kern::register_module_funcs(gsiop_functable, gsiop_functable_count);
    // call module entry: start(argc, argv)
    IOPContext c{};
    c.r[29] = 0x0001E000;
    c.r[28] = gsiop_gp;
    c.r[4] = 1;
    u32 argv = iop::alloc(64);
    u32 name = argv + 16;
    memcpy(mem::iop_ptr(name), "GS_IOP.IRX", 11);
    mem::iop_wr32(argv, name);
    c.r[5] = argv;
    (void)args;
    kern::call_func(&c, gsiop_entry);
    log_write(LOG_IOP, "GS_IOP: start returned %d", (int)c.r[2]);
    kern::run_ready();
}

}  // namespace iop::gs_iop
