"""Generate recompiled C code for all Grandia III EE code modules.

Usage: python gen_all.py <disc_dir> <out_dir>
"""
import hashlib
import json
import os
import struct
import sys

from codeinfo import Module, load_main_elf, load_overlay, discover_functions
from recomp import Recompiler, FuncResolver

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(HERE, '..', 'config', 'ee_config.json')


def load_config():
    if os.path.exists(CONFIG):
        with open(CONFIG) as f:
            return json.load(f)
    return {}


def find_sublayers(disc):
    """Find SUBLAYER.BIN overlays embedded in BTL/*.DAT; dedupe by content."""
    res = {}
    btl = os.path.join(disc, 'BTL')
    for fn in sorted(os.listdir(btl)):
        p = os.path.join(btl, fn)
        d = open(p, 'rb').read()
        # the file starts with a chunk table (count, then offset/size pairs); a sublayer's chunk also
        # holds the switch jump tables placed after the data the header counts
        chunks = {}
        n = struct.unpack_from('<I', d, 0)[0] if len(d) >= 4 else 0
        if 0 < n < 64 and 4 + 8 * n <= len(d):
            for k in range(n):
                o, s = struct.unpack_from('<II', d, 4 + 8 * k)
                chunks[o] = s
        pos = d.find(b'gccM')
        while pos != -1:
            if pos % 16 == 0 and d[pos + 0x20:pos + 0x2C] == b'SUBLAYER.BIN':
                magic, ver, load, text, data, bss, a6, a7 = struct.unpack_from('<4s7I', d, pos)
                img = d[pos:pos + 0x80 + text + data]
                h = hashlib.sha1(img).hexdigest()[:10]   # names stay as before the jump table fix
                if pos in chunks and chunks[pos] > len(img):
                    img = d[pos:pos + chunks[pos]]
                if h not in res:
                    res[h] = (fn, pos, load, text, data, bss, img)
            pos = d.find(b'gccM', pos + 1)
    return res


def main():
    disc = sys.argv[1]
    out = sys.argv[2]
    cfg = load_config()
    os.makedirs(out, exist_ok=True)

    mods = []
    sources = {}
    main_mod = load_main_elf(os.path.join(disc, 'SLUS_213.34'), 0x1E6000)
    mods.append(('main', 'ee', main_mod))
    # static initializer code placed after the data section (.init island)
    isl = main_mod.data[0x1F6C80 - main_mod.base:0x1F6CD4 - main_mod.base]
    mods.append(('main_init', 'eei', Module('main_init', 0x1F6C80, isl, 0x1F6CD4)))
    for name, fn in (('field', 'FIELD.BIN'), ('battle', 'BATTLE.BIN'), ('flight', 'FLIGHT.BIN')):
        m = load_overlay(os.path.join(disc, fn), name)
        mods.append((name, name, m))
        sources[name] = ('/' + fn, 0, len(m.data))
    subs = find_sublayers(disc)
    for h, (fn, pos, load, text, data, bss, img) in sorted(subs.items()):
        from codeinfo import extend_code_end
        m = Module('sub_' + h, load, img, extend_code_end(img[:0x80 + text + data], load, load + 0x80 + text), load + 0x80 + text + data,
                   load + 0x80 + text + data + bss, code_start=load + 0x80)
        mods.append(('sub_' + h, 'sub_' + h, m))
        sources['sub_' + h] = ('/BTL/' + fn, pos, len(img))
    print(f'{len(mods)} modules ({len(subs)} unique sublayers)')

    hle = {int(k, 16): v for k, v in cfg.get('hle', {}).items()}
    hooks = {int(k, 16): v for k, v in cfg.get('hooks', {}).items()}
    extra = {}
    for k, v in cfg.get('extra_entries', {}).items():
        extra.setdefault(k, []).extend(int(x, 16) for x in v)

    all_files = []
    manifest = []
    decl_headers = []
    # main module function names must be resolvable from overlays
    overlay_mods = tuple(m for n, p, m in mods if n != 'main')
    main_funcs, _, _, _ = discover_functions(main_mod, extra_starts=extra.get('main', []) + [0x100008],
                                             other_mods=overlay_mods)
    main_resolver_names = {f.start: f'ee_{f.start:08X}' for f in main_funcs}
    for name, prefix, mod in mods:
        if name == 'main':
            funcs = main_funcs
        else:
            funcs, _, _, _ = discover_functions(mod, extra_starts=extra.get(name, []))
        res = FuncResolver()
        res.local = dict(main_resolver_names)
        for f in funcs:
            res.local[f.start] = f'{prefix}_{f.start:08X}'
        if name == 'main':
            res.hle = hle
            res.hooks = hooks
        else:
            # overlays: HLE overrides and hooks only apply to main module functions they call
            res.hle = {a: n for a, n in hle.items() if main_mod.in_code(a)}
            res.hooks = {a: n for a, n in hooks.items() if main_mod.in_code(a)}
            # hooks on functions of this overlay ("module_hooks": {module: {addr: name}})
            res.hooks.update({int(k, 16): v for k, v in cfg.get('module_hooks', {}).get(name, {}).items()})
        rc = Recompiler(mod, funcs, res, prefix, cfg)
        files = rc.write(os.path.join(out, name))
        for w in rc.warnings:
            print(f'[{name}] warning: {w}')
        all_files.extend(files)
        decl_headers.append(f'{name}/{prefix}_decls.h')
        manifest.append({'name': name, 'prefix': prefix, 'base': mod.base, 'code_end': mod.code_end,
                         'data_end': mod.data_end, 'bss_end': mod.bss_end, 'nfuncs': len(funcs)})
        print(f'{name}: {len(funcs)} functions, {len(files)} files')

    # all_decls.h: only main decls (overlays call main functions directly)
    with open(os.path.join(out, 'all_decls.h'), 'w') as fh:
        fh.write('#pragma once\n#include "main/ee_decls.h"\n')
        fh.write('/* HLE replacement functions */\n')
        for a, n in sorted(hle.items()):
            fh.write(f'void {n}(EEContext* c);\n')
        fh.write('/* hooks (native code in front of a recompiled function) */\n')
        for a, n in sorted(hooks.items()):
            fh.write(f'void {n}(EEContext* c);\n')
        for mh in cfg.get('module_hooks', {}).values():
            for a, n in sorted(mh.items()):
                fh.write(f'void {n}(EEContext* c);\n')
    # module registry
    reg = ['#include "ee_recomp.h"', '#include "ee_modules.h"', '']
    for m in manifest:
        reg.append(f'extern const EEFuncEntry {m["prefix"]}_functable[];')
        reg.append(f'extern const unsigned {m["prefix"]}_functable_count;')
    reg.append('')
    reg.append('const EEModuleInfo g_ee_modules[] = {')
    for m in manifest:
        reg.append(f'    {{ "{m["name"]}", 0x{m["base"]:08X}u, 0x{m["code_end"]:08X}u, 0x{m["data_end"]:08X}u, '
                   f'0x{m["bss_end"]:08X}u, {m["prefix"]}_functable, &{m["prefix"]}_functable_count }},')
    reg.append('};')
    reg.append(f'const unsigned g_ee_module_count = {len(manifest)};')
    reg.append('const EEOverlaySource g_ee_overlay_sources[] = {')
    for name, (fn, off, size) in sources.items():
        reg.append(f'    {{ "{name}", "{fn}", 0x{off:X}u, 0x{size:X}u }},')
    reg.append('    { 0, 0, 0, 0 }')
    reg.append('};')
    with open(os.path.join(out, 'modules.c'), 'w') as fh:
        fh.write('\n'.join(reg) + '\n')
    all_files.append(os.path.join(out, 'modules.c'))
    with open(os.path.join(out, 'files.txt'), 'w') as fh:
        for f in all_files:
            fh.write(os.path.relpath(f, out).replace('\\', '/') + '\n')
    with open(os.path.join(out, 'manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=1)


if __name__ == '__main__':
    main()
