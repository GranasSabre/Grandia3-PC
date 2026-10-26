"""Build script: writes build.ninja and runs ninja.

Usage: python build.py [--debug] [--out=NAME] [--gen-only] [target]
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TC = os.path.join(ROOT, 'toolchain', 'llvm-mingw-20261006-ucrt-x86_64', 'bin')
NINJA = os.path.join(ROOT, 'toolchain', 'ninja', 'ninja.exe')


def p(path):
    return os.path.relpath(path, ROOT).replace('\\', '/')


def main():
    debug = '--debug' in sys.argv
    # --out=NAME builds into build/NAME with its own ninja file, so several builds can run concurrently
    out = next((a[6:] for a in sys.argv if a.startswith('--out=')), 'debug' if debug else 'release')
    bdir = f'build/{out}'
    opt = '-O1 -g' if debug else '-O2'
    if '--symbols' in sys.argv and not debug:
        opt += ' -g'  # optimized build with debug info, for line-level profiling (tools/perfmap.py --lines)
    common = f'{opt} -msse4.1 -mpopcnt -fwrapv -fno-strict-aliasing -ffp-contract=off -Iruntime/include -Igenerated'
    gen_flags = common + ' -w -Igenerated/main'
    if debug:
        gen_flags += ' -DEE_TRACE_FUNCS'
    cxx_flags = common + ' -std=c++20 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNOMINMAX -Wall -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-global -Wno-unused-but-set-variable -Iruntime/src'
    if debug:
        cxx_flags += ' -DGR3_DEBUG=1'
    ldflags = ('-static -mwindows' if not debug else '-static') + f' -Wl,-Map={bdir}/Grandia3.map'
    libs = '-ld3d11 -ldxgi -ld3dcompiler -lxinput1_4 -lole32 -lwinmm -luser32 -lgdi32 -lshell32 -lavrt -lksuser -luuid -lcomctl32 -lwindowscodecs -lcomdlg32'
    lines = [
        f'builddir = {bdir}',
        f'cc = {os.path.join(TC, "clang.exe")}',
        f'cxx = {os.path.join(TC, "clang++.exe")}',
        f'genflags = {gen_flags}',
        f'cxxflags = {cxx_flags}',
        f'ldflags = {ldflags}',
        f'libs = {libs}',
        'rule cc',
        '  command = $cc $genflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CC $in',
        'rule cxx',
        '  command = $cxx $cxxflags -MD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        '  description = CXX $in',
        'rule link',
        '  command = $cxx $ldflags -o $out @$out.rsp $libs',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '  description = LINK $out',
        'rule rc',
        '  command = ' + os.path.join(TC, 'windres.exe') + ' --include-dir runtime/res -O coff $in -o $out',
        '  description = RC $in',
        'rule ar',
        '  command = ' + os.path.join(TC, 'llvm-ar.exe') + ' rcs $out @$out.rsp',
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '  description = AR $out',
    ]
    objs_gen = []
    files_txt = os.path.join(ROOT, 'generated', 'files.txt')
    if os.path.exists(files_txt):
        for f in open(files_txt).read().split():
            src = 'generated/' + f
            obj = f'{bdir}/gen/' + f.replace('.c', '.o')
            lines.append(f'build {obj}: cc {src}')
            objs_gen.append(obj)
    import glob as _g
    for src in sorted(_g.glob(os.path.join(ROOT, 'generated', 'iop', '*.c'))):
        rel = p(src)
        obj = f'{bdir}/gen/' + rel[len('generated/'):].replace('.c', '.o')
        lines.append(f'build {obj}: cc {rel}')
        objs_gen.append(obj)
    objs_rt = []
    for src in sorted(glob.glob(os.path.join(ROOT, 'runtime', 'src', '**', '*.cpp'), recursive=True)):
        rel = p(src)
        obj = f'{bdir}/rt/' + rel[len('runtime/src/'):].replace('.cpp', '.o')
        lines.append(f'build {obj}: cxx {rel}')
        objs_rt.append(obj)
    for src in sorted(glob.glob(os.path.join(ROOT, 'runtime', 'src', '**', '*.c'), recursive=True)):
        rel = p(src)
        obj = f'{bdir}/rt/' + rel[len('runtime/src/'):].replace('.c', '.oc')
        lines.append(f'build {obj}: cc {rel}')
        objs_rt.append(obj)
    # translated VU microprograms register themselves via static initializers: link them directly
    # (objects in a static library would be dropped as unreferenced)
    for src in sorted(glob.glob(os.path.join(ROOT, 'generated', 'vu', '*.cpp'))):
        rel = p(src)
        obj = f'{bdir}/gen/vu/' + os.path.basename(rel).replace('.cpp', '.o')
        lines.append(f'build {obj}: cxx {rel}')
        objs_rt.append(obj)
    lines.append(f'build {bdir}/rt/grandia3_res.o: rc runtime/res/grandia3.rc | runtime/res/grandia3.manifest runtime/res/grandia3.ico')
    objs_rt.append(f'{bdir}/rt/grandia3_res.o')
    lines.append(f'build {bdir}/libgen.a: ar ' + ' '.join(objs_gen))
    exe = f'{bdir}/Grandia3.exe'
    lines.append(f'build {exe}: link ' + ' '.join(objs_rt) + f' {bdir}/libgen.a')
    lines.append(f'build gen: phony {bdir}/libgen.a')
    lines.append(f'default {exe}')
    ninja_file = os.path.join(ROOT, f'build-{out}.ninja')
    with open(ninja_file, 'w') as fh:
        fh.write('\n'.join(lines) + '\n')
    if '--gen-only' in sys.argv:
        return
    targets = [a for a in sys.argv[1:] if not a.startswith('--')]
    env = dict(os.environ)
    env['PATH'] = TC + os.pathsep + env['PATH']
    r = subprocess.call([NINJA, '-C', ROOT, '-f', ninja_file] + targets, env=env)
    sys.exit(r)


if __name__ == '__main__':
    main()
