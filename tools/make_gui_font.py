#!/usr/bin/env python3
"""make_gui_font.py FONT.ttf OUT.h : the SDL GUI's font (source/GuiFontDejaVu.h)

Renders ASCII 32..126 of a TrueType font (DejaVu Sans Mono, as used by
felixterminal: DejaVuSansMono.ttf, or the base64 in its DejaVuMono.h) at
10 pixels into 6x10 cells, anti-aliased, with coverage in 5 levels (0-4;
few in-between colours, so VGA mode 13h's palette isn't flooded). Baseline
at cell row 8. The font's own copyright and license (TrueType name table)
go into the header. Needs Pillow.
"""
import base64, re, struct, sys
from PIL import Image, ImageDraw, ImageFont

src, out = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
if src.endswith('.h'):   # felixterminal's base64 font header
    text = data.decode('ascii')
    m = re.search(r'static const char \w+_B64\[\]\s*=\s*((?:\s*"[^"]*")+)\s*;', text)
    data = base64.b64decode(''.join(re.findall(r'"([^"]*)"', m.group(1))))
    src = '/tmp/_gui_font.ttf'
    open(src, 'wb').write(data)

def name_record(font, want):
    """English name table string number `want`"""
    num = struct.unpack('>H', font[4:6])[0]
    for i in range(num):
        tag, _, off, _ = struct.unpack('>4sIII', font[12 + 16 * i:28 + 16 * i])
        if tag == b'name':
            _, count, strings = struct.unpack('>HHH', font[off:off + 6])
            for j in range(count):
                pid, eid, lang, nid, ln, so = struct.unpack('>6H', font[off + 6 + 12 * j:off + 18 + 12 * j])
                if nid == want and pid == 3 and lang == 0x409:
                    return font[off + strings + so:off + strings + so + ln].decode('utf-16-be')
    return ''

family = name_record(data, 4)
copyright = name_record(data, 0)
license = name_record(data, 13)

f = ImageFont.truetype(src, 10)
assert round(f.getlength('M')) == 6, 'expected a 6-pixel advance'
glyphs = []
for c in range(32, 127):
    im = Image.new('L', (6, 13), 0)
    ImageDraw.Draw(im).text((0, 0), chr(c), font=f, fill=255)
    glyphs.append([[(im.getpixel((x, y)) * 4 + 127) // 255 for x in range(6)] for y in range(2, 12)])

def comment(s):
    lines = []
    for para in s.replace('\r', '').split('\n'):
        words, line = para.split(), ''
        for w in words:
            if len(line) + len(w) + 1 > 74:
                lines.append(line); line = w
            else:
                line = (line + ' ' + w).strip()
        lines.append(line)
    return '\n'.join((' * ' + l).rstrip() for l in lines)

with open(out, 'w', newline='\n') as o:
    o.write('/*\n * GUI font: %s, rendered at 10 px into 6x10 cells by\n' % family)
    o.write(' * tools/make_gui_font.py (from felixterminal\'s DejaVuMono.h). Coverage\n')
    o.write(' * 0-4 per pixel; baseline at row 8; ASCII 32..126.\n *\n')
    o.write(comment(copyright) + '\n *\n' + comment(license) + '\n */\n')
    o.write('#ifndef GUI_FONT_DEJAVU_H\n#define GUI_FONT_DEJAVU_H\n\n')
    o.write('static const int GUI_FONT_W = 6, GUI_FONT_H = 10, GUI_FONT_LEVELS = 4;\n')
    o.write('// guiFont[ch - 32][row]: 6 pixels, 3 bits each (bit 0-2: leftmost)\n')
    o.write('static const unsigned int guiFont[95][10] = {\n')
    for c, g in zip(range(32, 127), glyphs):
        rows = []
        for r in g:
            v = 0
            for x, a in enumerate(r):
                v |= a << (3 * x)
            rows.append('0x%05x' % v)
        ch = chr(c)
        o.write(' {%s}, // %s\n' % (','.join(rows), repr(ch) if ch not in '\\' else "'\\\\'"))
    o.write('};\n\n#endif\n')
print('wrote', out)
