// Same content hash as the runtime's gs::texrep::hash (runtime/src/gs/gs_texrep.cpp), as a DLL
// for the Python pack tools: clang -O2 -shared -o texhash.dll texhash.c
#include <stdint.h>
#include <string.h>

__declspec(dllexport) uint64_t texhash(const void* data, size_t bytes, uint64_t seed) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed ^ (bytes * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}
