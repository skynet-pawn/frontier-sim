"""A small shapefile reader (polylines and polygons) and DBF reader, so the map tools need no GIS libraries.

read_dbf(path)          -> list of dicts (text fields are NUL-padded in Natural Earth's files; they are cleaned here)
read_shapes(path)       -> yields (record index, shape type, parts) where parts is a list of [(lon, lat), ...]
"""
import struct


def read_dbf(path):
    with open(path, 'rb') as f:
        data = f.read()
    n, hdr, rec = struct.unpack('<xxxxIHH', data[:12])
    fields, pos = [], 32
    while data[pos] != 0x0D:
        name = data[pos:pos + 11].split(b'\0')[0].decode('latin-1')
        fields.append((name, data[pos + 16]))
        pos += 32
    rows = []
    for i in range(n):
        base = hdr + i * rec + 1
        row, off = {}, 0
        for name, flen in fields:
            row[name] = data[base + off:base + off + flen].decode('utf-8', 'replace').replace(chr(0), '').strip()
            off += flen
        rows.append(row)
    return rows


def read_shapes(path):
    with open(path, 'rb') as f:
        data = f.read()
    pos, idx = 100, 0
    while pos + 8 <= len(data):
        _, clen = struct.unpack('>ii', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + clen * 2]
        pos += 8 + clen * 2
        shape_type = struct.unpack('<i', body[:4])[0]
        if shape_type in (3, 5, 13, 15, 23, 25):
            nparts, npts = struct.unpack('<ii', body[36:44])
            parts = list(struct.unpack('<%di' % nparts, body[44:44 + 4 * nparts]))
            pts_at = 44 + 4 * nparts
            pts = struct.unpack('<%dd' % (2 * npts), body[pts_at:pts_at + 16 * npts])
            xy = [(pts[2 * i], pts[2 * i + 1]) for i in range(npts)]
            yield idx, shape_type, [xy[a:b] for a, b in zip(parts, parts[1:] + [npts])]
        idx += 1
