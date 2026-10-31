"""Regenerate the vector fan mark and Windows icon using Python's standard library."""
from pathlib import Path
import math
import struct
import zlib

ASSETS = Path(__file__).resolve().parents[1] / 'assets'
SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
BLUE = (37, 99, 235, 255)
WHITE = (255, 255, 255, 255)
CYAN = (103, 232, 249, 255)
START = (126, 108)
CURVES = (
    ((108, 100), (86, 87), (90, 67)),
    ((94, 47), (118, 43), (136, 54)),
    ((155, 66), (154, 91), (142, 113)),
    ((137, 109), (131, 107), START),
)

def blade():
    points = [START]
    current = START
    for a, b, end in CURVES:
        for step in range(1, 25):
            t = step / 24
            points.append(tuple((1-t)**3 * current[i] + 3*(1-t)**2*t*a[i] +
                                3*(1-t)*t*t*b[i] + t**3*end[i] for i in (0, 1)))
        current = end
    return points

def rotate(points, angle):
    sine, cosine = math.sin(angle), math.cos(angle)
    return [(128+(x-128)*cosine-(y-128)*sine, 128+(x-128)*sine+(y-128)*cosine) for x, y in points]

def rounded_tile():
    points = []
    for x, y, start in ((192, 64, -90), (192, 192, 0), (64, 192, 90), (64, 64, 180)):
        for step in range(33):
            angle = math.radians(start + step * 90 / 32)
            points.append((x + 52 * math.cos(angle), y + 52 * math.sin(angle)))
    return points

def fill_polygon(buffer, size, points, color):
    points = [(x*size/256, y*size/256) for x, y in points]
    for y in range(max(0, int(min(p[1] for p in points))), min(size, math.ceil(max(p[1] for p in points)))):
        intersections = []
        for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
            if (y0 <= y+.5 < y1) or (y1 <= y+.5 < y0):
                intersections.append(x0 + (y+.5-y0)*(x1-x0)/(y1-y0))
        intersections.sort()
        for left, right in zip(intersections[::2], intersections[1::2]):
            first, last = max(0, math.ceil(left-.5)), min(size, math.ceil(right-.5))
            if last > first:
                buffer[4*(y*size+first):4*(y*size+last)] = bytes(color) * (last-first)

def render(size):
    scale = 4
    high = size * scale
    buffer = bytearray(high * high * 4)
    fill_polygon(buffer, high, rounded_tile(), BLUE)
    for angle in (0, 2*math.pi/3, 4*math.pi/3):
        fill_polygon(buffer, high, rotate(blade(), angle), WHITE)
    hub = [(128+13*math.cos(t*math.pi/64), 128+13*math.sin(t*math.pi/64)) for t in range(128)]
    fill_polygon(buffer, high, hub, CYAN)
    pixels = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            samples = [4*((y*scale+dy)*high+x*scale+dx) for dy in range(scale) for dx in range(scale)]
            alpha = sum(buffer[i+3] for i in samples)
            offset = 4*(y*size+x)
            if alpha:
                for channel in range(3):
                    pixels[offset+channel] = round(sum(buffer[i+channel]*buffer[i+3] for i in samples)/alpha)
                pixels[offset+3] = round(alpha/(scale*scale))
    return pixels

def png(size, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind+data))
    rows = b''.join(b'\0' + pixels[y*size*4:(y+1)*size*4] for y in range(size))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

def icon_bitmap(size, pixels):
    stride = ((size + 31)//32)*4
    bitmap, mask = bytearray(), bytearray(stride*size)
    for row, y in enumerate(range(size-1, -1, -1)):
        for x in range(size):
            r, g, b, a = pixels[(y*size+x)*4:(y*size+x+1)*4]
            bitmap.extend((b, g, r, a))
            if a == 0:
                mask[row*stride+x//8] |= 0x80 >> (x % 8)
    return struct.pack('<IiiHHIIiiII', 40, size, size*2, 1, 32, 0, len(bitmap)+len(mask), 0, 0, 0, 0) + bitmap + mask

def main():
    ASSETS.mkdir(exist_ok=True)
    path = 'M126 108 ' + ' '.join('C'+' '.join(str(v) for point in curve for v in point) for curve in CURVES) + ' Z'
    uses = '\n'.join(f'  <path d="{path}" fill="#fff" transform="rotate({angle} 128 128)"/>' for angle in (0, 120, 240))
    (ASSETS / 'app-icon.svg').write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">\n'
        '  <title>GPU Fan Controller</title>\n'
        '  <rect x="12" y="12" width="232" height="232" rx="52" fill="#2563eb"/>\n' + uses +
        '\n  <circle cx="128" cy="128" r="13" fill="#67e8f9"/>\n</svg>\n', encoding='utf-8')
    images = []
    for size in SIZES:
        pixels = render(size)
        images.append(icon_bitmap(size, pixels))
        if size == 256:
            (ASSETS / 'app-icon.png').write_bytes(png(size, pixels))
    offset = 6 + 16*len(SIZES)
    directory = bytearray(struct.pack('<HHH', 0, 1, len(SIZES)))
    for size, data in zip(SIZES, images):
        directory.extend(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    (ASSETS / 'app-icon.ico').write_bytes(directory + b''.join(images))
    print('Generated app-icon.svg, app-icon.png and app-icon.ico (16-256 px).')

if __name__ == '__main__':
    main()
