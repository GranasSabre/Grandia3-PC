#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

enum LogCat {
    LOG_MAIN, LOG_KERNEL, LOG_HW, LOG_DMA, LOG_GS, LOG_VIF, LOG_VU, LOG_SIF, LOG_IOP, LOG_CDVD,
    LOG_PAD, LOG_MC, LOG_SPU, LOG_IPU, LOG_GAME, LOG_COUNT
};

extern bool g_log_enabled[LOG_COUNT];
void log_write(int cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
std::string exe_dir();  // directory of the executable
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(cat, ...) do { if (g_log_enabled[cat]) log_write(cat, __VA_ARGS__); } while (0)
#define LOGK(...) LOG(LOG_KERNEL, __VA_ARGS__)

double host_time_sec();          // monotonic seconds since start
u64 host_time_us();
