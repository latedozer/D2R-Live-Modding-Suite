#!/usr/bin/env python3
"""Checks plugin.cpp's addresses against a dump of the game image it was written for:
the image stamp, every fingerprint in kPrints, and the functions the plugin calls.
usage: check_prints.py <plugin.cpp> <image dump (file offset == RVA)>"""
import re, struct, sys

source = open(sys.argv[1]).read()
try:
    image = open(sys.argv[2], 'rb').read()
except OSError:
    print('skip  no image dump at', sys.argv[2])
    sys.exit(0)
failed = 0
def check(ok, what):
    global failed
    print(('ok    ' if ok else 'FAIL  ') + what)
    failed += 0 if ok else 1

constants = {name: int(value, 16) for name, value in re.findall(r'constexpr \w+ (k\w+)\s*=\s*(0x[0-9a-fA-F]+);', source)}
stamp = struct.unpack_from('<I', image, struct.unpack_from('<I', image, 0x3c)[0] + 8)[0]
check(stamp == constants['kKnownStamp'], 'image stamp %08x is the one the plugin knows' % stamp)

prints = re.findall(r'\{ (k\w+|0x[0-9a-f]+), "([^"]+)", (true|false), (\d+), \{ ([^}]+) \} \}', source)
check(len(prints) == 6, '%d fingerprints found in plugin.cpp' % len(prints))
for where, what, entry, size, data in prints:
    rva = constants[where] if where in constants else int(where, 16)
    want = bytes(int(b, 16) for b in data.split(','))
    check(len(want) == int(size) and image[rva:rva + len(want)] == want, '%s at 0x%x' % (what, rva))

# the cube's own reader: count at +0x1578 (the vector's second field), record n = rows + n * 0x16c
check(image[0x3e3f2b:0x3e3f32] == bytes.fromhex('488b9878150000'), 'the count is read from data tables + 0x%x + 8' % constants['kVectorAt'])
check(constants['kVectorAt'] == 0x1570 and constants['kClassesAt'] == 0x18, 'vector and class table places')
# the cubemain loader hands these to the table reader: param -> kParamRva, mod N -> kModRva, class -> tables + 0x18
check(image[0x3e408e:0x3e4095] == bytes.fromhex('488d050b1d0000') and 0x3e4095 + 0x1d0b == constants['kParamRva'], 'the param column function is the one the loader names')
check(image[0x3e40b2:0x3e40b6] == bytes.fromhex('488d4618'), 'the class column reads the name table at data tables + 0x18')
loader = image[0x3e3ff0:0x3e51f2]
found = False
for at in range(len(loader) - 7):
    if loader[at] == 0x48 and loader[at + 1] == 0x8d and loader[at + 2] in (0x05, 0x0d, 0x15):
        target = 0x3e3ff0 + at + 7 + struct.unpack_from('<i', loader, at + 3)[0]
        found = found or target == constants['kModRva']
check(found, 'the mod column function is the one the loader names')
print('ALL PASSED' if failed == 0 else 'FAILURES: %d' % failed)
sys.exit(1 if failed else 0)
