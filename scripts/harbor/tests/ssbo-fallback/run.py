# SPDX-License-Identifier: GPL-3.0-or-later
import json,shlex,subprocess,pathlib,os
import sys
root=pathlib.Path(sys.argv[1]).resolve()
out=pathlib.Path(sys.argv[3]).resolve(); out.mkdir(parents=True,exist_ok=True)
build=pathlib.Path(sys.argv[2]).resolve()
entry=next(x for x in json.load(open(build/'compile_commands.json')) if x['file'].endswith('/spirv_emit_context.cpp'))
a=shlex.split(entry['command']); a=a[:a.index('-o')]; a=[x for x in a if not x.startswith('-W') and x!='-O3']; a+=['-O0','-ffunction-sections','-fdata-sections']
source=root/'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp'
a+=['-I'+str(root/'src')]
source=pathlib.Path(os.environ.get('SSBO_TEST_SOURCE',source))
for name,src in [('context',source),('harness',root/'scripts/harbor/tests/ssbo-fallback/emit.cpp')]:
 subprocess.run(a+['-c',str(src),'-o',str(out/(name+'.o'))],check=True)
libs=[build/'src/shader_recompiler/libshader_recompiler.a',build/'_deps/sirit-build/src/libsirit.a',build/'src/common/libcommon.a']
subprocess.run(['c++',str(out/'harness.o'),str(out/'context.o'),'-Wl,--gc-sections','-Wl,--start-group',*map(str,libs),'-Wl,--end-group','-lfmt','-lzstd','-llz4','-lssl','-lcrypto','-lz','-pthread','-o',str(out/'ssbo')],check=True)
subprocess.run([str(out/'ssbo'),str(out/'ssbo.spv')],check=True)

subprocess.run(['spirv-dis','--raw-id',str(out/'ssbo.spv'),'-o',str(out/'ssbo.spvasm')],check=True)
subprocess.run([sys.executable,str(root/'scripts/harbor/tests/ssbo-fallback/check.py'),str(out/'ssbo.spvasm')],check=True)
