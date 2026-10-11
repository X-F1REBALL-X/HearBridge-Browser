#!/usr/bin/env python3
"""embed.py IN OUT SYMBOL GUARD -> C header with SYMBOL[] and SYMBOL_LEN."""
import pathlib, sys
src, dst, sym, guard = sys.argv[1:5]
data = pathlib.Path(src).read_bytes()
out = [f'/* AUTO-GENERATED from {pathlib.Path(src).name} — do not edit by hand. */',
       f'#ifndef {guard}', f'#define {guard}', f'#define {sym}_LEN {len(data)}',
       f'static const char {sym}[{sym}_LEN + 1] =']
out.append('\n'.join('    "' + ''.join('\\x%02x' % b for b in data[i:i+60]) + '"'
                     for i in range(0, len(data), 60)) + ';')
out.append('#endif')
pathlib.Path(dst).write_text('\n'.join(out) + '\n')
print(f'embed: {pathlib.Path(src).name} {len(data)} bytes')
