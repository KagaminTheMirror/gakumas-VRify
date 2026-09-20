from pathlib import Path
import argparse
import struct
parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
root = parser.parse_args().root
data = (root / 'build/bin/x64/Release/version.dll').read_bytes()
u16 = lambda off: struct.unpack_from('<H', data, off)[0]
u32 = lambda off: struct.unpack_from('<I', data, off)[0]
pe = u32(0x3c)
assert data[pe:pe+4] == b'PE\0\0'
optional = pe+24
assert u16(optional) == 0x20b
section_start = optional+u16(pe+20)
sections = [struct.unpack_from('<IIII',data,section_start+i*40+8) for i in range(u16(pe+6))]
def offset(rva):
    for size, address, raw_size, raw in sections:
        if address <= rva < address+max(size,raw_size): return raw+rva-address
    raise AssertionError(f'Unknown RVA: {rva}')
directory = offset(u32(optional+112))
names = offset(u32(directory+32))
exports=set()
for i in range(u32(directory+24)):
    start=offset(u32(names+i*4))
    exports.add(data[start:data.index(b'\0',start)].decode())
expected=set('GetFileVersionInfoA GetFileVersionInfoByHandle GetFileVersionInfoExA GetFileVersionInfoExW GetFileVersionInfoSizeA GetFileVersionInfoSizeExA GetFileVersionInfoSizeExW GetFileVersionInfoSizeW GetFileVersionInfoW VerFindFileA VerFindFileW VerInstallFileA VerInstallFileW VerLanguageNameA VerLanguageNameW VerQueryValueA VerQueryValueW'.split())
assert {x for x in exports if not x.startswith('GakumasVr')} == expected, exports
print(f'PASS: all {len(expected)} accepted version.dll proxy exports, no DXGI exports')
