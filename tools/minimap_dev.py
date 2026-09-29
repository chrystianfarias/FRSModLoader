"""Development dump for the minimap mod: the game's map textures and its races.

    python tools/minimap_dev.py [--game F:/Games/NFSU2] [--out build/minimap-dev]

Writes, for LOOKING at while the mod is being built:

    textures/<NAME>.png   TRACKMAP4000 (free roam), a few race TRACKMAPs and
                          every MINIMAP_* icon in InGameCommon.bun, with alpha
    events.json           every race start: trigger, world x/y/z, races
    events-on-map.png     the triggers drawn on TRACKMAP4000, to check the
                          world -> map framing

None of this ships. The mod itself reads the same textures out of the game's
packs at runtime (http://nfsu2.tex/<NAME>), so a texture mod shows up in it and
nothing extracted from the game ever sits in a mod folder.

Formats, from NOTES.md and FRSWorldEditor's NFSU2-FORMATS.md:
  - texture pack: bChunk B3300000; entries 124 bytes in 33310004, formats in
    33310005, pixels in 33320002 from the next 128-byte boundary
  - races: GLOBALB.BUN career 0x80034A10 -> strings 0x34A1D, races 0x34A11
    (136 bytes; name offset, track id, trigger hash)
  - triggers: zones in 0x3414A of PathsFreeRoam.bin, hash at +0x30
"""
import argparse
import collections
import json
import os
import re
import struct

from PIL import Image, ImageDraw

# world -> TRACKMAP pixel, same framing on every L4RA city map (512x512)
MAP_K, MAP_CX, MAP_CY = 0.0917, 312.0, 298.5

KINDS = ['CIRCUIT', 'SPRINT', 'DRIFT', 'DRAG', 'STREET', 'URL', 'SUV']
COLOUR = {'CIRCUIT': (230, 25, 75), 'SPRINT': (60, 180, 75), 'DRIFT': (70, 200, 240),
          'DRAG': (245, 130, 48), 'STREET': (240, 50, 230), 'URL': (255, 225, 25),
          'SUV': (170, 110, 40)}


def bin_hash(s):
    h = 0xFFFFFFFF
    for c in s.encode('latin1') if isinstance(s, str) else s:
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def chunks(buf, off, end):
    """(id, data offset, data size); 0x11 padding up to the alignment is skipped."""
    while off + 8 <= end:
        cid, size = struct.unpack_from('<II', buf, off)
        raw = off + 8
        if raw + size > end:
            return
        align = 128 if cid == 0x33320002 else 16
        data = (raw + align - 1) // align * align
        if data > raw + size or any(b != 0x11 for b in buf[raw:data]):
            data = raw
        yield cid, data, size - (data - raw)
        off = raw + size


# ---------------------------------------------------------------- textures --

def textures(path):
    buf = open(path, 'rb').read()
    out = {}
    for cid, d, s in chunks(buf, 0, len(buf)):
        if cid != 0xB3300000:
            continue
        hdr = fmt = pix = None
        for c1, d1, s1 in chunks(buf, d, d + s):
            for c2, d2, s2 in chunks(buf, d1, d1 + s1) if c1 in (0xB3310000, 0xB3320000) else ():
                if c2 == 0x33310004:
                    hdr = (d2, s2)
                elif c2 == 0x33310005:
                    fmt = (d2, s2)
                elif c2 == 0x33320002:
                    pix = d2
        if not (hdr and fmt and pix is not None):
            continue
        for i in range(hdr[1] // 124):
            e, g = hdr[0] + i * 124, fmt[0] + i * 32
            name = buf[e + 0x0C:buf.find(b'\0', e + 0x0C)].decode('latin1')
            w, h = struct.unpack_from('<HH', buf, e + 0x44)
            at, pal, size, pal_size = struct.unpack_from('<4I', buf, e + 0x30)
            out[name] = dict(name=name, width=w, height=h,
                             fourcc=struct.unpack_from('<I', buf, g + 0x14)[0],
                             data=buf[pix + at:pix + at + size],
                             palette=buf[pix + pal:pix + pal + 1024] if pal_size else b'')
    return out


def rgb565(v):
    return ((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)


def decode(t):
    """RGBA bytes, or None for a format this does not know."""
    w, h, data, four = t['width'], t['height'], t['data'], t['fourcc']
    out = bytearray(w * h * 4)
    if four in (0x31545844, 0x33545844, 0x35545844):          # DXT1, DXT3, DXT5
        dxt1 = four == 0x31545844
        stride = 8 if dxt1 else 16
        bw = max(1, (w + 3) // 4)
        for b in range(len(data) // stride):
            o = b * stride
            alpha = [255] * 16
            if four == 0x33545844:
                bits = int.from_bytes(data[o:o + 8], 'little')
                alpha = [(bits >> (4 * k) & 15) * 17 for k in range(16)]
            elif four == 0x35545844:
                a0, a1 = data[o], data[o + 1]
                bits = int.from_bytes(data[o + 2:o + 8], 'little')
                if a0 > a1:
                    lut = [a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)]
                else:
                    lut = [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255]
                alpha = [lut[bits >> (3 * k) & 7] for k in range(16)]
            if not dxt1:
                o += 8
            c0, c1, idx = struct.unpack_from('<HHI', data, o)
            p = [rgb565(c0), rgb565(c1)]
            if c0 > c1 or not dxt1:
                p += [tuple((2 * p[0][k] + p[1][k]) // 3 for k in range(3)),
                      tuple((p[0][k] + 2 * p[1][k]) // 3 for k in range(3))]
                transparent = -1
            else:
                p += [tuple((p[0][k] + p[1][k]) // 2 for k in range(3)), (0, 0, 0)]
                transparent = 3
            bx, by = b % bw * 4, b // bw * 4
            for k in range(16):
                x, y = bx + k % 4, by + k // 4
                if x < w and y < h:
                    n = idx >> (2 * k) & 3
                    q = (y * w + x) * 4
                    out[q:q + 4] = bytes(p[n]) + bytes([0 if n == transparent else alpha[k]])
        return bytes(out)
    if four == 21 or (four == 0 and len(data) >= w * h * 4):   # A8R8G8B8
        for i in range(w * h):
            b_, g_, r_, a_ = data[i * 4:i * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((r_, g_, b_, a_))
        return bytes(out)
    if four == 41 and len(t['palette']) >= 1024:              # P8
        pal = t['palette']
        for i in range(min(len(data), w * h)):
            e = data[i] * 4
            out[i * 4:i * 4 + 4] = bytes((pal[e + 2], pal[e + 1], pal[e], pal[e + 3]))
        return bytes(out)
    return None


def save(t, folder):
    rgba = decode(t)
    if rgba is None:
        print('  %-32s format 0x%X not decoded' % (t['name'], t['fourcc']))
        return None
    img = Image.frombytes('RGBA', (t['width'], t['height']), rgba)
    if img.getchannel('A').getextrema() == (0, 0):
        # the TRACKMAPs are DXT3 with every alpha nibble at zero: the game
        # draws them opaque, so the alpha block carries nothing
        img.putalpha(255)
    img.save(os.path.join(folder, t['name'] + '.png'))
    print('  %-32s %4dx%-4d fourcc 0x%X' % (t['name'], t['width'], t['height'], t['fourcc']))
    return img


# ------------------------------------------------------------------- races --

def races(game):
    g = open(os.path.join(game, 'GLOBAL', 'GLOBALB.BUN'), 'rb').read()
    for cid, d, s in chunks(g, 0, len(g)):
        if cid == 0x80034A10:          # the first career chunk is the full career
            sub = {c: (dd, ss) for c, dd, ss in chunks(g, d, d + s)}
            break
    else:
        raise RuntimeError('no career chunk in GLOBALB.BUN')
    sd, ss = sub[0x34A1D]
    table = g[sd:sd + ss]
    text = lambda o: table[o:table.find(b'\0', o)].decode('latin1')
    names = {bin_hash(m): m.decode() for m in re.findall(rb'[\x20-\x7e]+', table)}
    rd, rs = sub[0x34A11]
    out = []
    for i in range(rs // 136):
        w = struct.unpack_from('<34I', g, rd + i * 136)
        out.append(dict(name=text(w[0]), track=w[6] & 0xFFFF, trigger_hash=w[10],
                        trigger=names.get(w[10], '%08X' % w[10])))
    # the kind is in the name; unnamed races take the kind of their track
    kind_of = lambda n: next((k for k in KINDS if re.search(r'(^|_)%s(_|$)' % k, n)), None)
    by_track = {r['track']: kind_of(r['name']) for r in out if kind_of(r['name'])}
    for r in out:
        r['kind'] = kind_of(r['name']) or by_track.get(r['track'], 'CIRCUIT')
    return out


def triggers(game, hashes):
    b = open(os.path.join(game, 'TRACKS', 'ROUTESL4RA', 'PathsFreeRoam.bin'), 'rb').read()
    out = {}
    for h in hashes:
        at = b.find(struct.pack('<I', h))
        if at < 0x30:
            continue
        kind, x, y = struct.unpack_from('<Iff', b, at - 0x30)
        x0, y0, x1, y1 = struct.unpack_from('<4f', b, at - 0x10)
        if kind == 0x0D and x0 <= x <= x1 and y0 <= y <= y1:
            out[h] = dict(x=x, y=y, z=struct.unpack_from('<f', b, at - 0x1C)[0])
    return out


# -------------------------------------------------------------------- main --

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='F:/Games/NFSU2')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'build', 'minimap-dev'))
    a = ap.parse_args()
    tex_dir = os.path.join(a.out, 'textures')
    os.makedirs(tex_dir, exist_ok=True)

    print('InGameCommon.bun:')
    common = textures(os.path.join(a.game, 'GLOBAL', 'InGameCommon.bun'))
    for name in sorted(common):
        if re.search(r'MINIMAP|MAPBACK|WORLD_MAP|GPS', name):
            save(common[name], tex_dir)

    print('TRACKMAPs:')
    maps = {}
    for tid in ('4000', '4011', '4061', '4041'):
        for t in textures(os.path.join(a.game, 'TRACKS', 'TRACKMAP%s.BIN' % tid)).values():
            maps[t['name']] = save(t, tex_dir)

    rs = [r for r in races(a.game) if not r['name'].startswith('DDAY')]
    trig = triggers(a.game, {r['trigger_hash'] for r in rs})
    events = collections.OrderedDict()
    for r in sorted(rs, key=lambda r: r['trigger']):
        z = trig.get(r['trigger_hash'])
        if not z:
            continue
        e = events.setdefault(r['trigger'], dict(trigger=r['trigger'], **z, races=[]))
        e['races'].append(dict(name=r['name'], track=r['track'], kind=r['kind']))
    doc = dict(framing=dict(k=MAP_K, cx=MAP_CX, cy=MAP_CY,
                            note='px = k*x + cx, py = -k*y + cy on a 512x512 L4RA TRACKMAP'),
               events=list(events.values()))
    with open(os.path.join(a.out, 'events.json'), 'w') as f:
        json.dump(doc, f, indent=1)
    print('%d races -> %d start points (events.json)' %
          (sum(len(e['races']) for e in events.values()), len(events)))

    base = maps.get('TRACKMAP4000')
    if base:
        up = 2
        img = Image.new('RGBA', base.size, (0, 0, 0, 255))
        img.alpha_composite(base)
        img = img.resize((base.width * up, base.height * up), Image.LANCZOS)
        d = ImageDraw.Draw(img)
        for e in events.values():
            px, py = (MAP_K * e['x'] + MAP_CX) * up, (-MAP_K * e['y'] + MAP_CY) * up
            c = COLOUR[collections.Counter(r['kind'] for r in e['races']).most_common(1)[0][0]]
            d.ellipse([px - 6, py - 6, px + 6, py + 6], fill=c, outline=(0, 0, 0))
        img.save(os.path.join(a.out, 'events-on-map.png'))
        print('events-on-map.png')


if __name__ == '__main__':
    main()
