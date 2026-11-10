#!/usr/bin/env python3
from pathlib import Path
import argparse,json,subprocess,hashlib
p=argparse.ArgumentParser();p.add_argument('--kit',type=Path,required=True);p.add_argument('--ndk',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();r=Path(__file__).resolve().parents[1];tool=a.ndk.resolve()/'toolchains/llvm/prebuilt/linux-x86_64/bin'
objects=[a.kit/'objects'/x['object'] for x in json.loads((r/'tools/receiver-units.json').read_text())];libs=[a.kit/'deps'/x for x in json.loads((r/'tools/library-order.json').read_text())]
assert all(p.is_file() for p in objects+libs),'Incomplete relink kit'
assert not a.output.exists(),'Refuse overwriting existing output'
cmd=[tool/'aarch64-linux-android28-clang','-pthread','-Wl,--no-undefined,--gc-sections,--build-id=sha1','-o',a.output]+objects+['-Wl,--start-group']+libs+['-Wl,--end-group','-lm','-lz','-ldl','-latomic']
subprocess.run([str(x) for x in cmd],check=True);subprocess.run([str(tool/'llvm-strip'),'--strip-debug',str(a.output)],check=True)
print(hashlib.sha256(a.output.read_bytes()).hexdigest(),a.output.name)
