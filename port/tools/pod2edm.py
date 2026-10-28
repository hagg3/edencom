#!/usr/bin/env python3
"""pod2edm.py — convert Eden's creature models from PowerVR .pod to the engine's own .edm (LG.7).

The seven media/models/*.pod files were exported by the PowerVR SDK's 3ds Max/Maya plug-in and read
at run time by the SDK's CPVRTModelPOD. LG.7 (2026-10-27) removed the SDK, so this tool converts
them once, offline, into `.edm`, which Classes/CreatureModel.cpp reads. The `.pod` files stay in the
repo as the source art; only the `.edm` files ship.

What is kept is exactly what the engine reads, in the form the SDK evaluated it:
  - each mesh's interleaved vertex block (uploaded to a VBO as-is), the five attributes the creature
    draw binds (position, normal, UV0, bone indices, bone weights: type/count/stride/offset), the
    triangle list (u16), and the bone batches;
  - each node's mesh index, parent and animation flags, and its position / rotation / scale keys.
    An animated channel is written as one key per frame, with the SDK's per-frame index arrays
    (pnAnim*Idx) already resolved; a static channel is one key. Scale keeps x,y,z of the SDK's
    7-float scale. The run-time interpolation (lerp, quaternion slerp) is in CreatureModel.cpp.
Everything else in a .pod (materials, textures, cameras, lights, names, export history) is dropped.

The tool refuses what the seven models do not use rather than half-converting it: strips, matrix
animation, the old node format, fixed-point scenes, non-interleaved meshes.

Format (.edm v1, little-endian, every field 4 bytes unless noted; docs/entities-and-creatures.md):
  "EDM1"  u32 version=1  u32 numFrame  u32 numMesh  u32 numNode  u32 numMeshNode
  mesh x numMesh:
    u32 numVertex  u32 numFaces
    5 x {u32 type, u32 n, u32 stride, u32 offset}      position, normal, uv0, boneIdx, boneWeight
    u32 bytes, u8[bytes] interleaved, pad to 4
    u32 count, u16[count] indices, pad to 4
    i32 batchBoneMax  i32 batchCnt  i32[batchCnt*batchBoneMax] batches  i32[batchCnt] boneCnt  i32[batchCnt] offset
  node x numNode:
    i32 meshIdx  i32 parent  u32 animFlags
    u32 keys, f32[keys*3] position;  u32 keys, f32[keys*4] rotation (x,y,z,w);  u32 keys, f32[keys*3] scale

Usage:
  pod2edm.py media/models/Moof.pod [...]          write Moof.edm beside each input
  pod2edm.py --all                                every media/models/*.pod (run from anywhere)
  pod2edm.py --check                              exit 1 if any committed .edm differs from a fresh conversion
"""
import glob
import os
import struct
import sys

END = 0x80000000
POD_VERSION = b'AB.POD.2.0\0'
HAS_POS, HAS_ROT, HAS_SCALE, HAS_MATRIX = 0x1, 0x2, 0x4, 0x8
TYPE_FLOAT, TYPE_USHORT, TYPE_UBYTE = 1, 3, 10


class PodError(Exception):
    pass


class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def marker(self):
        if self.o + 8 > len(self.b):
            return None
        n, l = struct.unpack_from('<II', self.b, self.o)
        self.o += 8
        return n, l

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.o)[0]
        self.o += 4
        return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.o)[0]
        self.o += 4
        return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]
        if len(v) != n:
            raise PodError('truncated')
        self.o += n
        return v


def read_data(r, spec, valid):
    d = {'type': 0, 'n': 0, 'stride': 0, 'data': None}
    while True:
        n, l = r.marker()
        if n == (spec | END):
            return d
        if n == 9000:
            d['type'] = r.u32()
        elif n == 9001:
            d['n'] = r.u32()
        elif n == 9002:
            d['stride'] = r.u32()
        elif n == 9003:
            d['data'] = r.raw(l) if valid else r.u32()   # interleaved: a byte offset
        else:
            r.raw(l)


ATTR_TAGS = {6003: 'faces', 6006: 'vtx', 6007: 'nor', 6008: 'tan', 6009: 'bin', 6010: 'uvw',
             6011: 'col', 6012: 'bidx', 6013: 'bw'}


def read_mesh(r):
    m = {'uvw': [], 'interleaved': None, 'strips': 0, 'bonemax': 0, 'nbatch': 0,
         'batches': (), 'batchcnt': (), 'batchoff': ()}
    while True:
        n, l = r.marker()
        if n == (2012 | END):
            return m
        if n == 6000: m['nv'] = r.u32()
        elif n == 6001: m['nf'] = r.u32()
        elif n == 6002: m['nuvw'] = r.u32()
        elif n == 6005: m['strips'] = r.u32()
        elif n == 6014: m['interleaved'] = r.raw(l)
        elif n == 6015: m['batches'] = struct.unpack('<%di' % (l // 4), r.raw(l))
        elif n == 6016: m['batchcnt'] = struct.unpack('<%di' % (l // 4), r.raw(l))
        elif n == 6017: m['batchoff'] = struct.unpack('<%di' % (l // 4), r.raw(l))
        elif n == 6018: m['bonemax'] = r.u32()
        elif n == 6019: m['nbatch'] = r.u32()
        elif n in ATTR_TAGS:
            d = read_data(r, n, n == 6003 or m['interleaved'] is None)
            if ATTR_TAGS[n] == 'uvw':
                m['uvw'].append(d)
            else:
                m[ATTR_TAGS[n]] = d
        else:
            r.raw(l)


NODE_ARRAYS = {5007: 'pos', 5008: 'rot', 5009: 'scale', 5011: 'matrix',
               5013: 'posidx', 5014: 'rotidx', 5015: 'scaleidx', 5016: 'matrixidx'}


def read_node(r):
    nd = {'flags': 0, 'parent': -1, 'idx': -1}
    while True:
        n, l = r.marker()
        if n == (2013 | END):
            return nd
        if n == 5000: nd['idx'] = r.i32()
        elif n == 5003: nd['parent'] = r.i32()
        elif n == 5012: nd['flags'] = r.u32()
        elif n in NODE_ARRAYS:
            k = NODE_ARRAYS[n]
            nd[k] = struct.unpack('<%d%s' % (l // 4, 'I' if k.endswith('idx') else 'f'), r.raw(l))
        elif n in (5004, 5005, 5006, 5010):
            raise PodError('old node format (pos/rot/scale/matrix tags) is not supported')
        else:
            r.raw(l)


def read_pod(path):
    r = Reader(open(path, 'rb').read())
    scene, version_ok = None, False
    while True:
        m = r.marker()
        if m is None:
            break
        n, l = m
        if n == 1000:
            version_ok = r.raw(l) == POD_VERSION
            continue
        if n == 1001:
            s = {'meshes': [], 'nodes': [], 'flags': 0}
            while True:
                n2, l2 = r.marker()
                if n2 == (1001 | END):
                    break
                if n2 == 2004: s['nmesh'] = r.u32()
                elif n2 == 2005: s['nnode'] = r.u32()
                elif n2 == 2006: s['nmeshnode'] = r.u32()
                elif n2 == 2009: s['nframe'] = r.u32()
                elif n2 == 2016: s['flags'] = r.u32()
                elif n2 == 2012: s['meshes'].append(read_mesh(r))
                elif n2 == 2013: s['nodes'].append(read_node(r))
                else: r.raw(l2)
            scene = s
            continue
        r.raw(l)
    if not version_ok:
        raise PodError('not an AB.POD.2.0 file')
    if scene is None:
        raise PodError('no scene')
    if scene['flags'] & 1:
        raise PodError('fixed-point scene')
    if len(scene['meshes']) != scene['nmesh'] or len(scene['nodes']) != scene['nnode']:
        raise PodError('mesh/node count mismatch')
    return scene


def channel_keys(nd, name, comps, stride, animated, nframe):
    """The keys the SDK's Get*Matrix would read, one per frame (animated) or one (static)."""
    data = nd.get(name)
    if data is None:
        raise PodError('node without a %s channel' % name)
    if not animated:
        if len(data) < comps:
            raise PodError('short static %s' % name)
        return [data[:comps]]
    idx = nd.get(name + 'idx')
    keys = []
    for f in range(nframe):
        at = idx[f] if idx is not None else stride * f
        if at + comps > len(data):
            raise PodError('%s key %d out of range' % (name, f))
        keys.append(data[at:at + comps])
    return keys


ATTR_ORDER = ('vtx', 'nor', 'uv0', 'bidx', 'bw')


def to_edm(scene):
    nframe = scene['nframe']
    if nframe < 2:
        raise PodError('fewer than 2 frames')
    out = bytearray(b'EDM1')
    out += struct.pack('<5I', 1, nframe, scene['nmesh'], scene['nnode'], scene['nmeshnode'])
    for m in scene['meshes']:
        if m['strips']:
            raise PodError('triangle strips are not supported')
        if m['interleaved'] is None:
            raise PodError('non-interleaved mesh')
        f = m['faces']
        if f['type'] != TYPE_USHORT:
            raise PodError('faces are not u16')
        m['uv0'] = m['uvw'][0] if m['uvw'] else {'type': 0, 'n': 0, 'stride': 0, 'data': 0}
        out += struct.pack('<2I', m['nv'], m['nf'])
        for k in ATTR_ORDER:
            a = m.get(k) or {'type': 0, 'n': 0, 'stride': 0, 'data': 0}
            # The SDK tested `pData != NULL`, i.e. offset != 0, for the bone arrays; the engine now tests
            # n != 0. The two agree as long as a present array never sits at offset 0.
            if a['n'] and k in ('bidx', 'bw') and a['data'] == 0:
                raise PodError('bone array at offset 0')
            out += struct.pack('<4I', a['type'], a['n'], a['stride'], a['data'] or 0)
        il = m['interleaved']
        out += struct.pack('<I', len(il)) + il + b'\0' * (-len(il) % 4)
        idx = f['data']
        if len(idx) != m['nf'] * 3 * 2:
            raise PodError('index count != 3 * faces')
        out += struct.pack('<I', len(idx) // 2) + idx + b'\0' * (-len(idx) % 4)
        bmax, bcnt = m['bonemax'], m['nbatch']
        if len(m['batches']) != bcnt * bmax or len(m['batchcnt']) != bcnt or len(m['batchoff']) != bcnt:
            raise PodError('bone batch arrays do not match their counts')
        out += struct.pack('<2i', bmax, bcnt)
        for arr in (m['batches'], m['batchcnt'], m['batchoff']):
            out += struct.pack('<%di' % len(arr), *arr)
    for nd in scene['nodes']:
        flags = nd['flags']
        if flags & HAS_MATRIX or 'matrix' in nd:
            raise PodError('matrix animation is not supported')
        out += struct.pack('<iiI', nd['idx'], nd['parent'], flags)
        for name, comps, stride, bit in (('pos', 3, 3, HAS_POS), ('rot', 4, 4, HAS_ROT), ('scale', 3, 7, HAS_SCALE)):
            keys = channel_keys(nd, name, comps, stride, bool(flags & bit), nframe)
            out += struct.pack('<I', len(keys))
            for k in keys:
                out += struct.pack('<%df' % comps, *k)
    return bytes(out)


def models_dir():
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'media', 'models')


def main(argv):
    check = '--check' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if '--all' in argv or check:
        paths = sorted(glob.glob(os.path.join(models_dir(), '*.pod')))
    if not paths:
        print(__doc__)
        return 2
    bad = 0
    for p in paths:
        dst = os.path.splitext(p)[0] + '.edm'
        try:
            data = to_edm(read_pod(p))
        except PodError as e:
            print('%s: %s' % (p, e), file=sys.stderr)
            bad += 1
            continue
        if check:
            cur = open(dst, 'rb').read() if os.path.exists(dst) else None
            if cur != data:
                print('%s: differs from a fresh conversion of %s' % (dst, os.path.basename(p)), file=sys.stderr)
                bad += 1
            continue
        with open(dst, 'wb') as fh:
            fh.write(data)
        print('%s -> %s (%d bytes)' % (os.path.basename(p), os.path.basename(dst), len(data)))
    if check and not bad:
        print('pod2edm --check: %d models match' % len(paths))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
