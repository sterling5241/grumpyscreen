#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12/screen-apps
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12
#
# Based on fb-http.py from https://github.com/paxx12/screen-apps, modified for
# grumpyscreen: the screen is shown upright and touches land where they were
# aimed for any DISPLAY_ROTATE and touch calibration grumpyscreen runs with,
# and it streams mjpeg rather than polling png, which is a lot cheaper on a
# printer cpu.
"""Serve the grumpyscreen display as a camera, and touches on it as touches.

    GET  /                  a page showing the screen that passes touches on
    GET  /stream            the screen as an mjpeg stream
    GET  /snapshot          the screen as a jpeg
    POST /touch?a=&x=&y=    a = down, move, up or tap, at a point of the
                            screen as shown

Point a moonraker webcam at it:

    [webcam screen]
    service: iframe
    stream_url: /screen/
    snapshot_url: /screen/snapshot
"""

import argparse
import ctypes
import fcntl
import glob
import hashlib
import json
import mmap
import os
import threading
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from io import BytesIO
from urllib.parse import urlparse, parse_qs
import struct

from PIL import Image

FBIOGET_VSCREENINFO = 0x4600
FBIOGET_FSCREENINFO = 0x4602

class FbVarScreeninfo(ctypes.Structure):
    _fields_ = [
        ("xres", ctypes.c_uint32),
        ("yres", ctypes.c_uint32),
        ("xres_virtual", ctypes.c_uint32),
        ("yres_virtual", ctypes.c_uint32),
        ("xoffset", ctypes.c_uint32),
        ("yoffset", ctypes.c_uint32),
        ("bits_per_pixel", ctypes.c_uint32),
        ("grayscale", ctypes.c_uint32),
        ("red", ctypes.c_uint32 * 3),
        ("green", ctypes.c_uint32 * 3),
        ("blue", ctypes.c_uint32 * 3),
        ("transp", ctypes.c_uint32 * 3),
        ("nonstd", ctypes.c_uint32),
        ("activate", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("accel_flags", ctypes.c_uint32),
        ("pixclock", ctypes.c_uint32),
        ("left_margin", ctypes.c_uint32),
        ("right_margin", ctypes.c_uint32),
        ("upper_margin", ctypes.c_uint32),
        ("lower_margin", ctypes.c_uint32),
        ("hsync_len", ctypes.c_uint32),
        ("vsync_len", ctypes.c_uint32),
        ("sync", ctypes.c_uint32),
        ("vmode", ctypes.c_uint32),
        ("rotate", ctypes.c_uint32),
        ("colorspace", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32 * 4),
    ]

class FbFixScreeninfo(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char * 16),
        ("smem_start", ctypes.c_ulong),
        ("smem_len", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("type_aux", ctypes.c_uint32),
        ("visual", ctypes.c_uint32),
        ("xpanstep", ctypes.c_uint16),
        ("ypanstep", ctypes.c_uint16),
        ("ywrapstep", ctypes.c_uint16),
        ("line_length", ctypes.c_uint32),
        ("mmio_start", ctypes.c_ulong),
        ("mmio_len", ctypes.c_uint32),
        ("accel", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint16),
        ("reserved", ctypes.c_uint16 * 2),
    ]

EV_SYN = 0x00
EV_KEY = 0x01
EV_ABS = 0x03
SYN_REPORT = 0x00
BTN_TOUCH = 0x14a
ABS_X = 0x00
ABS_Y = 0x01
ABS_MT_SLOT = 0x2f
ABS_MT_TRACKING_ID = 0x39
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36

def log(msg):
    ts = time.strftime('%H:%M:%S')
    print(f"[{ts}] {msg}", flush=True)

class Grumpyscreen:
    """What the running grumpyscreen was started with: its service sets
    DISPLAY_ROTATE and LVGL_EVDEV_DEV, and calibration.json sits next to it."""

    def __init__(self, rotate=None, touch=None):
        self.env = {}
        self.dir = None
        for path in glob.glob('/proc/[0-9]*/cmdline'):
            try:
                with open(path, 'rb') as f:
                    exe = f.read().split(b'\0')[0].decode(errors='ignore')
                if os.path.basename(exe) != 'grumpyscreen':
                    continue
                with open(path.replace('cmdline', 'environ'), 'rb') as f:
                    env = f.read().split(b'\0')
                self.env = dict(e.decode(errors='ignore').split('=', 1) for e in env if b'=' in e)
                self.dir = os.path.dirname(exe)
                break
            except OSError:
                continue

        try:
            self.rotate = rotate if rotate is not None else int(self.env.get('DISPLAY_ROTATE', 0)) % 4
        except ValueError:
            self.rotate = 0
        self.touch = touch or self.env.get('LVGL_EVDEV_DEV') or '/dev/input/event0'
        self.calibration = self._calibration()
        log(f"grumpyscreen: rotate={self.rotate}, touch={self.touch}, calibrated={self.calibration is not None}")

    def _calibration(self):
        if not self.dir:
            return None
        try:
            with open(os.path.join(self.dir, 'calibration.json')) as f:
                c = [float(v) for v in json.load(f)['calibrations']]
            return c if len(c) == 6 else None
        except (OSError, ValueError, KeyError, TypeError):
            return None

class Framebuffer:
    def __init__(self, device='/dev/fb0', rotate=0, quality=70):
        self.device = device
        self.rotate = rotate
        self.quality = quality
        self.fd = None
        self.mm = None
        self.width = 0
        self.height = 0
        self.virtual_width = 0
        self.virtual_height = 0
        self.bpp = 0
        self.red_offset = 16
        self.line_length = 0
        self._cache_hash = None
        self._cache_jpeg = None
        # the stream and snapshots share the mapping from several threads
        self._lock = threading.Lock()
        self._open()

    def _open(self):
        self.fd = os.open(self.device, os.O_RDONLY)
        vinfo = FbVarScreeninfo()
        fcntl.ioctl(self.fd, FBIOGET_VSCREENINFO, vinfo)
        finfo = FbFixScreeninfo()
        fcntl.ioctl(self.fd, FBIOGET_FSCREENINFO, finfo)

        self.width = vinfo.xres
        self.virtual_width = vinfo.xres_virtual
        self.height = vinfo.yres
        self.virtual_height = vinfo.yres_virtual
        self.bpp = vinfo.bits_per_pixel
        self.red_offset = vinfo.red[0]
        self.line_length = finfo.line_length

        size = self.line_length * self.virtual_height
        self.mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        log(f"Framebuffer: {self.width}x{self.height} ({self.virtual_width}x{self.virtual_height} virtual) @ {self.bpp}bpp, line_length={self.line_length}")

    def get_snapshot(self, client_etag=None):
        with self._lock:
            return self._get_snapshot(client_etag)

    def _get_snapshot(self, client_etag):
        vinfo = FbVarScreeninfo()
        fcntl.ioctl(self.fd, FBIOGET_VSCREENINFO, vinfo)
        offset = vinfo.yoffset * self.line_length

        self.mm.seek(offset)
        raw = self.mm.read(self.line_length * self.height)
        raw_hash = hashlib.md5(raw).hexdigest()[:16]
        if client_etag and client_etag == raw_hash:
            return raw_hash, None
        if raw_hash == self._cache_hash and self._cache_jpeg:
            return raw_hash, self._cache_jpeg
        if self.bpp == 32:
            rawmode = 'BGRX' if self.red_offset == 16 else 'RGBX'
            img = Image.frombytes('RGB', (self.width, self.height), raw, 'raw', rawmode, self.line_length)
        elif self.bpp == 16:
            img = Image.frombytes('RGB', (self.width, self.height), raw, 'raw', 'BGR;16', self.line_length)
        else:
            img = Image.frombytes('RGB', (self.width, self.height), raw, 'raw', 'BGR', self.line_length)
        # undo lvgl's software rotation so the screen is upright
        if self.rotate == 1:
            img = img.transpose(Image.ROTATE_270)
        elif self.rotate == 2:
            img = img.transpose(Image.ROTATE_180)
        elif self.rotate == 3:
            img = img.transpose(Image.ROTATE_90)
        buf = BytesIO()
        img.save(buf, 'JPEG', quality=self.quality)
        jpeg_data = buf.getvalue()
        self._cache_hash = raw_hash
        self._cache_jpeg = jpeg_data
        return raw_hash, jpeg_data

    def close(self):
        if self.mm:
            self.mm.close()
        if self.fd:
            os.close(self.fd)

class TouchInput:
    def __init__(self, grumpyscreen, fb_width, fb_height):
        self.device = grumpyscreen.touch
        self.rotate = grumpyscreen.rotate
        self.calibration = grumpyscreen.calibration
        self.fd = None
        self.fb_width = fb_width
        self.fb_height = fb_height
        self.multitouch = True
        self._open()

    def _open(self):
        try:
            self.fd = os.open(self.device, os.O_WRONLY)
            self._get_abs_info()
            log(f"Touch device: {self.device}, multitouch={self.multitouch}")
        except OSError as e:
            log(f"Failed to open touch device: {e}")
            self.fd = None

    def _get_abs_info(self):
        # the kernel drops events a device does not report, so position with
        # the codes it does
        EVIOCGABS = lambda axis: 0x80184540 + axis
        try:
            buf = bytearray(24)
            fcntl.ioctl(self.fd, EVIOCGABS(ABS_MT_POSITION_X), buf)
        except OSError:
            self.multitouch = False

    def _write_event(self, ev_type, code, value):
        if self.fd is None:
            return
        tv_sec = int(time.time())
        tv_usec = int((time.time() % 1) * 1000000)
        event = struct.pack('llHHi', tv_sec, tv_usec, ev_type, code, value)
        os.write(self.fd, event)

    def _scale(self, x, y):
        """A point on the upright screen to raw touch coordinates, undoing
        lvgl's indev_pointer_proc rotation and grumpyscreen's calibration.
        Without calibration grumpyscreen takes raw touch as panel pixels."""
        w, h = self.fb_width, self.fb_height
        if self.rotate == 1:
            x, y = y, h - 1 - x
        elif self.rotate == 2:
            x, y = w - 1 - x, h - 1 - y
        elif self.rotate == 3:
            x, y = w - 1 - y, x
        if self.calibration:
            a, b, c, d, e, f = self.calibration
            det = a * e - b * d
            if det:
                px, py = x - c, y - f
                x, y = (e * px - b * py) / det, (a * py - d * px) / det
        return max(0, int(round(x))), max(0, int(round(y)))

    def _position(self, touch_x, touch_y):
        if self.multitouch:
            self._write_event(EV_ABS, ABS_MT_POSITION_X, touch_x)
            self._write_event(EV_ABS, ABS_MT_POSITION_Y, touch_y)
        else:
            self._write_event(EV_ABS, ABS_X, touch_x)
            self._write_event(EV_ABS, ABS_Y, touch_y)

    def tap(self, x, y):
        if self.fd is None:
            log(f"Touch device not available, would tap at ({x}, {y})")
            return
        self.touch_down(x, y)
        time.sleep(0.05)
        self.touch_up()

    def touch_down(self, x, y):
        if self.fd is None:
            return
        touch_x, touch_y = self._scale(x, y)
        log(f"Touch down at ({x}, {y}) -> touch ({touch_x}, {touch_y})")
        if self.multitouch:
            self._write_event(EV_ABS, ABS_MT_SLOT, 0)
            # lvgl's evdev driver only takes tracking id 0 as a press
            self._write_event(EV_ABS, ABS_MT_TRACKING_ID, 0)
        self._position(touch_x, touch_y)
        self._write_event(EV_KEY, BTN_TOUCH, 1)
        self._write_event(EV_SYN, SYN_REPORT, 0)

    def touch_move(self, x, y):
        if self.fd is None:
            return
        touch_x, touch_y = self._scale(x, y)
        self._position(touch_x, touch_y)
        self._write_event(EV_SYN, SYN_REPORT, 0)

    def touch_up(self):
        if self.fd is None:
            return
        log(f"Touch up")
        if self.multitouch:
            self._write_event(EV_ABS, ABS_MT_TRACKING_ID, -1)
        self._write_event(EV_KEY, BTN_TOUCH, 0)
        self._write_event(EV_SYN, SYN_REPORT, 0)

    def close(self):
        if self.fd:
            os.close(self.fd)

PAGE = b"""<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  html, body { margin: 0; padding: 0; width: 100%; height: 100%; background: transparent; overflow: hidden; }
  img { width: 100%; height: 100%; object-fit: contain; touch-action: none; user-select: none; -webkit-user-drag: none; }
</style>
</head>
<body>
<img id="screen" src="stream">
<script>
  const img = document.getElementById('screen');
  let down = false;

  // object-fit: contain letterboxes the screen inside the element
  function point(event) {
    const w = img.naturalWidth, h = img.naturalHeight;
    if (!w || !h) return null;
    const rect = img.getBoundingClientRect();
    const scale = Math.min(rect.width / w, rect.height / h);
    const x = Math.round((event.clientX - rect.left - (rect.width - w * scale) / 2) / scale);
    const y = Math.round((event.clientY - rect.top - (rect.height - h * scale) / 2) / scale);
    return { x: Math.max(0, Math.min(w - 1, x)), y: Math.max(0, Math.min(h - 1, y)) };
  }

  function send(action, p) {
    fetch('touch?a=' + action + '&x=' + p.x + '&y=' + p.y, { method: 'POST' }).catch(() => {});
  }

  img.addEventListener('pointerdown', e => {
    const p = point(e);
    if (!p) return;
    down = true;
    img.setPointerCapture(e.pointerId);
    send('down', p);
  });
  img.addEventListener('pointermove', e => {
    const p = down && point(e);
    if (p) send('move', p);
  });
  img.addEventListener('pointerup', e => {
    const p = down && point(e);
    down = false;
    if (p) send('up', p);
  });
</script>
</body>
</html>
"""

class ScreenHandler(BaseHTTPRequestHandler):
    framebuffer = None
    touch_input = None
    fps = 5

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ('/', '/index.html'):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', len(PAGE))
            self.end_headers()
            self.wfile.write(PAGE)
        elif path == '/snapshot':
            self.handle_snapshot()
        elif path == '/stream':
            self.handle_stream()
        else:
            self.send_error(404, 'Not Found')

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == '/touch':
            self.handle_touch(parsed.query)
        else:
            self.send_error(404, 'Not Found')

    def handle_snapshot(self):
        try:
            client_etag = self.headers.get('If-None-Match', '').strip('"')
            etag, jpeg_data = self.framebuffer.get_snapshot(client_etag)
            if jpeg_data is None:
                self.send_response(304)
                self.send_header('ETag', f'"{etag}"')
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('Content-Type', 'image/jpeg')
            self.send_header('Content-Length', len(jpeg_data))
            self.send_header('ETag', f'"{etag}"')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            self.wfile.write(jpeg_data)
        except Exception as e:
            log(f"Snapshot error: {e}")
            self.send_error(500, str(e))

    def handle_stream(self):
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-cache')
        self.end_headers()
        try:
            while True:
                _, jpeg_data = self.framebuffer.get_snapshot()
                self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\n')
                self.wfile.write(f'Content-Length: {len(jpeg_data)}\r\n\r\n'.encode())
                self.wfile.write(jpeg_data + b'\r\n')
                time.sleep(1.0 / self.fps)
        except (OSError, ValueError):
            pass

    def handle_touch(self, query):
        try:
            params = parse_qs(query)
            action = params.get('a', ['tap'])[0]
            x = int(params.get('x', [0])[0])
            y = int(params.get('y', [0])[0])
            if action == 'down':
                self.touch_input.touch_down(x, y)
            elif action == 'move':
                self.touch_input.touch_move(x, y)
            elif action == 'up':
                self.touch_input.touch_up()
            else:
                self.touch_input.tap(x, y)
            response = f'{{"status":"ok","a":"{action}","x":{x},"y":{y}}}'.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', len(response))
            self.end_headers()
            self.wfile.write(response)
        except Exception as e:
            log(f"Touch error: {e}")
            response = f'{{"status":"error","message":"{e}"}}'.encode()
            self.send_response(500)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', len(response))
            self.end_headers()
            self.wfile.write(response)

def main():
    parser = argparse.ArgumentParser(description='grumpyscreen screen camera')
    parser.add_argument('-p', '--port', type=int, default=8092, help='HTTP port')
    parser.add_argument('--bind', default='0.0.0.0', help='Bind address')
    parser.add_argument('--fb', default='/dev/fb0', help='Framebuffer device')
    parser.add_argument('--touch', help='Touch input device, default is the one grumpyscreen uses')
    parser.add_argument('--rotate', type=int, choices=range(4), help='Default is grumpyscreen\'s DISPLAY_ROTATE')
    parser.add_argument('--fps', type=float, default=5, help='Stream frames per second')
    parser.add_argument('--quality', type=int, default=70, help='JPEG quality')
    args = parser.parse_args()

    grumpyscreen = Grumpyscreen(args.rotate, args.touch)
    fb = Framebuffer(args.fb, grumpyscreen.rotate, args.quality)
    touch = TouchInput(grumpyscreen, fb.width, fb.height)

    ScreenHandler.framebuffer = fb
    ScreenHandler.touch_input = touch
    ScreenHandler.fps = args.fps

    server = ThreadingHTTPServer((args.bind, args.port), ScreenHandler)
    log(f"Server running on http://{args.bind}:{args.port}")
    log(f"  GET  /           - HTML viewer")
    log(f"  GET  /stream     - MJPEG stream")
    log(f"  GET  /snapshot   - JPEG snapshot")
    log(f"  POST /touch?a=&x=&y= - Send touch event")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("Shutting down...")
    finally:
        fb.close()
        touch.close()

if __name__ == '__main__':
    main()
