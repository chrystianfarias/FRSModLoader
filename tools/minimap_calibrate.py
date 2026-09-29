"""Calibration for the redrawn TRACKMAPs the minimap carries.

    python tools/minimap_calibrate.py --pack "<NFSU2 Detailed Map v1 Reup.zip>" [--game ...] [--maps ...]

The minimap frames each map with the calibration in the track's TrackInfo
(GLOBALB.BUN chunk 0x34201: +0xAC top-left x, +0xB0 top-left y, +0xB4 width in
world units). "NFSU2 Detailed Map" redraws some maps with a different framing -
the URL circuit, for one, is moved into an enlarged inset - and expects its own
GLOBALB edits, which the minimap does not make: the game's files stay as they
are. So each redrawn map is checked against the race it belongs to instead:

  1. the race's route (TRACKS/ROUTES<region>/Paths<id>.bin, chunk 0x34148) is
     drawn with the game's calibration over the redrawn map, and the brightness
     under it is measured - a race map paints its route white;
  2. only the tracks whose calibration the pack's script.end edits are
     refitted. Tracks sharing one edit share one fit, and only the params the
     script touches move (OffsetX/Y the corner, ZoomIn the width): the
     script's numbers themselves do not frame the maps as this reads them,
     but which fields it touches is a reliable constraint;
  3. everything else keeps the game's calibration, and its route brightness
     is printed as a check.

Writes <maps>/calibration.json: {"<id>": {"ulx", "uly", "width"}}, read by the
page when it uses the redrawn map. Tracks with no route (4000, free roam) keep
the game's calibration.
"""
import argparse
import glob
import json
import os
import struct

from PIL import Image

def chunks(b, o, e):
    while o + 8 <= e:
        i, s = struct.unpack_from('<II', b, o)
        yield i, o + 8, s
        o += 8 + s


def find(b, o, e, cid):
    for i, d, s in chunks(b, o, e):
        if i == cid:
            return d, s
        if i & 0x80000000:
            r = find(b, d, d + s, cid)
            if r:
                return r
    return None


def track_infos(game):
    g = open(os.path.join(game, 'GLOBAL', 'GLOBALB.BUN'), 'rb').read()
    at = find(g, 0, len(g), 0x34201)
    out = {}
    d, s = at
    for o in range(d, d + s - 295, 296):
        tid = struct.unpack_from('<H', g, o + 0x8A)[0]
        if tid:
            ulx, uly, w = struct.unpack_from('<3f', g, o + 0xAC)
            out[tid] = dict(region=g[o + 0x40:o + 0x44].decode('latin1'), ulx=ulx, uly=uly, width=w)
    return out


def route(game, region, tid):
    p = os.path.join(game, 'TRACKS', 'ROUTES' + region, 'Paths%d.bin' % tid)
    if not os.path.exists(p):
        return []
    b = open(p, 'rb').read()
    at = find(b, 0, len(b), 0x34148)
    if not at:
        return []
    d, s = at
    return [struct.unpack_from('<2f', b, d + k * 24) for k in range(s // 24)]


def score(img, pts, ulx, uly, w):
    W, H = img.size
    tot = n = 0
    for x, y in pts:
        px, py = int((x - ulx) / w * W), int((uly - y) / w * H)
        if 0 <= px < W and 0 <= py < H:
            tot += img.getpixel((px, py))
            n += 1
    # a route that leaves the map scores what it lost
    return tot / len(pts) if pts else 0, n


FIELDS = {'TrackMapCalibrationOffsetX': 0, 'TrackMapCalibrationOffsetY': 1}


def pack_edits(pack):
    """{track id: set of calibration params the pack's script edits}. The
    script's own numbers are not usable as such (NOTES.md), but WHICH fields it
    touches, and which tracks share one edit, is what constrains the fit."""
    import zipfile
    z = zipfile.ZipFile(pack)
    script = next(n for n in z.namelist() if n.lower().endswith('script.end'))
    edits = {}
    for line in z.read(script).decode('latin1').splitlines():
        p = line.split()
        if len(p) >= 6 and p[0] == 'update_collection' and p[4].startswith('TrackMapCalibration'):
            edits.setdefault(int(p[3]), {})[p[4]] = p[5]
    return edits


def group_score(members, c):
    return sum(score(img, pts, *c)[0] for img, pts in members) / len(members)


def fit(members, start, free):
    """Coordinate search over the free params (0 ulx, 1 uly, 2 width), shared by
    every member of the group; a box match seeds it when the width is free."""
    best = tuple(start)
    if 2 in free:
        img, pts = members[0]
        W = img.size[0]
        bright = [(x, y) for y in range(0, W, 2) for x in range(0, W, 2) if img.getpixel((x, y)) > 235]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        if bright:
            bx, by = [p[0] for p in bright], [p[1] for p in bright]
            a = ((max(bx) - min(bx)) / max(1, max(xs) - min(xs)) +
                 (max(by) - min(by)) / max(1, max(ys) - min(ys))) / 2
            seed = (min(xs) - min(bx) / a, max(ys) + min(by) / a, W / a)
            if group_score(members, seed) > group_score(members, best):
                best = seed
    elif free == {1}:
        # one free param: scan it across the whole map height first
        best = max(((best[0], best[1] + d, best[2]) for d in range(-4000, 4001, 25)),
                   key=lambda c: group_score(members, c))
    step = best[2] * 0.01
    while step > best[2] * 0.0002:
        improved = False
        for k in sorted(free):
            for sgn in (-1, 1):
                c = list(best)
                c[k] += sgn * step
                if group_score(members, c) > group_score(members, best):
                    best, improved = tuple(c), True
        if not improved:
            step /= 2
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='F:/Games/NFSU2')
    ap.add_argument('--maps', default=os.path.join(os.path.dirname(__file__), '..', 'mods', 'minimap', 'maps'))
    ap.add_argument('--pack', required=True, help='the Detailed Map zip, for its script.end')
    a = ap.parse_args()
    infos = track_infos(a.game)
    edits = pack_edits(a.pack)
    have = {int(os.path.basename(f)[8:12]): f for f in glob.glob(os.path.join(a.maps, 'TRACKMAP*.dds'))}

    # everything the pack does not recalibrate must fit as the game frames it
    for tid in sorted(set(have) - set(edits)):
        t = infos.get(tid)
        pts = route(a.game, t['region'], tid) if t else []
        if pts:
            img = Image.open(have[tid]).convert('L').resize((1024, 1024), Image.BOX)
            print('%d  game calibration, route brightness %.0f' % (tid, score(img, pts, t['ulx'], t['uly'], t['width'])[0]))

    groups = {}
    for tid, e in edits.items():
        if tid in have:
            groups.setdefault(tuple(sorted(e.items())), []).append(tid)
    out = {}
    for key, tids in groups.items():
        t0 = infos[tids[0]]
        members = []
        for tid in tids:
            pts = route(a.game, infos[tid]['region'], tid)
            if pts:
                members.append((Image.open(have[tid]).convert('L').resize((1024, 1024), Image.BOX), pts))
        free = {FIELDS[k] for k, _ in key if k in FIELDS}
        if any(k == 'TrackMapCalibrationZoomIn' for k, _ in key):
            free.add(2)       # the pack rescales these maps: the width moves too
        start = (t0['ulx'], t0['uly'], t0['width'])
        s0 = group_score(members, start)
        c = fit(members, start, free)
        s1 = group_score(members, c)
        print('group %s (%d routes): %s  %.0f -> %.0f  ulx %.1f uly %.1f width %.1f' %
              (tids, len(members), dict(key), s0, s1, *c))
        for tid in tids:
            out[str(tid)] = dict(ulx=round(c[0], 1), uly=round(c[1], 1), width=round(c[2], 1))
    with open(os.path.join(a.maps, 'calibration.json'), 'w') as fp:
        json.dump(out, fp, indent=1, sort_keys=True)
    print('%d overrides written' % len(out))


if __name__ == '__main__':
    main()
