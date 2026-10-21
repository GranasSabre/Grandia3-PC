// HLE of padman (libpad 2.x protocol): RPC commands + periodic pad data DMA into the EE pad area.
#include "pad.h"
#include "../savestate.h"
#include "iop.h"
#include "modules.h"
#include "../memory.h"
#include "../hw/sif.h"
#include "../platform/platform.h"

namespace iop::pad {

struct Port {
    bool open = false;
    u32 area = 0;
    u32 frame = 0;
    u8 mode_id = 0x73;  // analog
    bool locked = false;
    int which = 0;
    u8 align[6] = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF};  // actuator of each SetActDirect byte
};
static Port g_ports[2];

static void write_frame(int p) {
    Port& pt = g_ports[p];
    if (!pt.open || !pt.area) return;
    u8 f[128];
    memset(f, 0, sizeof(f));
    platform::PadState ps = platform::read_pad(p);
    u16 inv = (u16)~ps.buttons;
    if (ps.buttons) LOG(LOG_PAD, "pad %d buttons %04x (frame %u)", p, ps.buttons, pt.frame);
    f[0] = 0x00;
    f[1] = pt.mode_id;
    f[2] = (u8)inv;
    f[3] = (u8)(inv >> 8);
    f[4] = ps.rx;
    f[5] = ps.ry;
    f[6] = ps.lx;
    f[7] = ps.ly;
    if (pt.mode_id == 0x79)
        for (int i = 0; i < 12; i++) f[8 + i] = ps.pressure[i];
    // actuator info (DualShock: small and large motor)
    f[48] = 0x00; f[49] = 0x01; f[50] = 0x01; f[51] = 0x01;
    f[52] = 0x00; f[53] = 0x02; f[54] = 0x00; f[55] = 0x01;
    u16 modes[4] = {0x0004, 0x0007, 0, 0};
    memcpy(f + 80, modes, 8);
    pt.frame++;
    memcpy(f + 88, &pt.frame, 4);
    u32 len = 32;
    memcpy(f + 96, &len, 4);
    f[100] = 2;        // modeConfig: mode switching + actuators supported (DualShock 2)
    f[101] = pt.mode_id;
    f[102] = 3;        // model: DualShock 2
    f[103] = 1;        // button data ready
    f[104] = 2;        // number of modes
    f[105] = pt.mode_id == 0x41 ? 0 : 1;
    f[106] = 2;        // actuators
    f[107] = 1;
    f[114] = 1;        // currentTask = update pad (this libpad's InfoMode/InfoAct check it, else return 0)
    f[112] = 6;        // PAD_STATE_STABLE
    f[113] = 0;        // PAD_RSTAT_COMPLETE
    sif::ee_write(pt.area + pt.which * 128, f, 128);
    pt.which ^= 1;
}

void vblank() {
    for (int p = 0; p < 2; p++) write_frame(p);
}

void register_servers() {
    auto handler = [](u32 fno, u32 buf, u32 size) -> u32 {
        (void)fno;
        (void)size;
        u32* b = (u32*)mem::iop_ptr(buf);
        u32 cmd = b[0];
        switch (cmd) {
        case 0x10:  // INIT
            b[3] = 1;
            break;
        case 0x12:  // GET_MODVER
            b[3] = 0x0400;
            break;
        case 0x01: {  // OPEN(port, slot, area)
            u32 port = b[1] & 1;
            g_ports[port] = Port();
            g_ports[port].open = (b[2] == 0);
            g_ports[port].area = b[4] & 0x1FFFFFFF;
            log_write(LOG_PAD, "scePadPortOpen(%u, %u, %08x)", b[1], b[2], b[4]);
            write_frame(port);
            write_frame(port);
            b[3] = 1;
            b[5] = 0x1000 + port * 0x100;
            break;
        }
        case 0x06: {  // SET_MMODE(port, slot, mode, lock)
            u32 port = b[1] & 1;
            g_ports[port].mode_id = b[3] ? 0x73 : 0x41;
            g_ports[port].locked = (b[4] == 3);
            LOG(LOG_PAD, "scePadSetMainMode(%u, mode=%u, lock=%u)", port, b[3], b[4]);
            b[5] = 1;
            break;
        }
        case 0x07: case 0x08: {  // SET_ACTDIR / SET_ACTALIGN(port, slot, 6 bytes)
            u32 port = b[1] & 1;
            const u8* d = (const u8*)&b[3];
            static int nlog = 0;
            if (nlog++ < 12) {
                char t[64];
                snprintf(t, sizeof(t), "pad: %s", cmd == 7 ? "SetActDirect" : "SetActAlign");
                modules::hexdump(LOG_PAD, t, (u8*)b, 32);
            }
            if (cmd == 0x08) memcpy(g_ports[port].align, d, 6);
            else {
                // DualShock: actuator 0 = small motor (on/off), 1 = large motor (0..255)
                u8 small = 0, large = 0;
                for (int i = 0; i < 6; i++) {
                    if (g_ports[port].align[i] == 0) small = d[i];
                    else if (g_ports[port].align[i] == 1) large = d[i];
                }
                if (port == 0) platform::set_vibration(small ? 1.0f : 0.0f, large / 255.0f);
            }
            b[5] = 1;
            break;
        }
        case 0x09: b[3] = 0x3FFFF; break;   // GET_BTNMASK
        case 0x0A: b[3] = 1; break;         // SET_BTNMASK
        case 0x0B: b[3] = 1; break;         // SET_VREF
        case 0x0C: b[3] = 2; break;         // GET_PORTMAX
        case 0x0D: b[3] = 1; break;         // GET_SLOTMAX
        case 0x0E: g_ports[b[1] & 1].open = false; b[3] = 1; break;  // CLOSE
        case 0x0F: b[3] = 1; break;         // END
        default: {
            char t[64];
            snprintf(t, sizeof(t), "pad: unknown cmd %08x", cmd);
            modules::hexdump(LOG_PAD, t, (u8*)b, 32);
            b[3] = 1;
            break;
        }
        }
        return buf;
    };
    register_native_server(0x80000100, 0x100, handler);
    register_native_server(0x80000101, 0x100, handler);
}

void start(const std::string&) {}

}  // namespace iop::pad

namespace iop::pad {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& p : g_ports) s.pod(p, "pad::Port");
}
}  // namespace iop::pad
