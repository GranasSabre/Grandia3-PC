// Native replacements for selected guest functions (see config/ee_config.json).
#include "../common.h"
#include "ee_recomp.h"

extern "C" {

// Generic: return 0
void hle_ret0(EEContext* c) { c->r[2].sd[0] = 0; }

}
