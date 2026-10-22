#!/usr/bin/env python3
"""Simulate what the 880x528 black/white/red e-paper panel will show.

It runs the same decode logic as sketch/hebclk.ino (PNG -> RGB565 -> black plane
+ red plane) and paints the two planes in e-paper colours, so you can check the
result without hardware.

Usage:
  python tools/simulate_panel.py                    # render locally, write panel_preview.png
  python tools/simulate_panel.py --url http://localhost:8765/clock.png
  python tools/simulate_panel.py --serve            # live preview at http://localhost:8800/
"""
import argparse
import io
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

PANEL_W, PANEL_H = 880, 528
PAPER, INK, RED = (232, 229, 218), (25, 25, 25), (190, 35, 30)


def get_png(url: str | None, args) -> bytes:
    if url:
        with urllib.request.urlopen(url, timeout=15) as r:
            return r.read()
    from app.services.clock import generate_clock_image
    return generate_clock_image(args.font, args.sleep, None if args.no_weather else
                                {"temp": 22, "desc": "מעונן חלקית", "icon_key": "sun_cloud"})


def decode_like_sketch(png: bytes):
    """Mirror of pngDrawCallback(): returns (black_plane, red_plane) as bytearrays, 1 = white."""
    im = Image.open(io.BytesIO(png)).convert("RGB")
    if im.size != (PANEL_W, PANEL_H):
        raise SystemExit(f"Wrong dimensions: {im.size}, expected {PANEL_W}x{PANEL_H}")
    row_bytes = PANEL_W // 8
    black = bytearray(b"\xff" * (row_bytes * PANEL_H))
    red = bytearray(b"\xff" * (row_bytes * PANEL_H))
    px = im.load()
    for y in range(PANEL_H):
        for x in range(PANEL_W):
            r8, g8, b8 = px[x, y]
            r, g, b = r8 >> 3, g8 >> 2, b8 >> 3
            p = (r << 11) | (g << 5) | b
            idx = y * row_bytes + x // 8
            if r >= 20 and g < 24 and b < 12:
                red[idx] &= ~(0x80 >> (x % 8)) & 0xFF
            elif p < 0x8000:
                black[idx] &= ~(0x80 >> (x % 8)) & 0xFF
    return black, red


def paint(black: bytearray, red: bytearray) -> Image.Image:
    out = Image.new("RGB", (PANEL_W, PANEL_H), PAPER)
    px = out.load()
    row_bytes = PANEL_W // 8
    for y in range(PANEL_H):
        for x in range(PANEL_W):
            i, m = y * row_bytes + x // 8, 0x80 >> (x % 8)
            if not red[i] & m:
                px[x, y] = RED
            elif not black[i] & m:
                px[x, y] = INK
    return out


def render(args) -> Image.Image:
    black, red = decode_like_sketch(get_png(args.url, args))
    print(f"planes OK: {len(black)} + {len(red)} bytes (panel buffer size)")
    return paint(black, red)


PAGE = """<!doctype html><meta charset=utf-8><title>Panel simulator</title>
<meta http-equiv=refresh content=30>
<body style="margin:0;background:#222;display:grid;place-items:center;min-height:100vh">
<img src="/panel.png?{t}" style="max-width:96vw;border:14px solid #111;border-radius:6px"></body>"""


def serve(args):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a): pass

        def do_GET(self):
            if self.path.startswith("/panel.png"):
                buf = io.BytesIO()
                render(args).save(buf, "PNG")
                body, ctype = buf.getvalue(), "image/png"
            else:
                body, ctype = PAGE.format(t=int(time.time())).encode(), "text/html"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.end_headers()
            self.wfile.write(body)

    print(f"Simulator: http://localhost:{args.port}/")
    HTTPServer(("0.0.0.0", args.port), H).serve_forever()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", help="fetch the PNG from a running server instead of rendering locally")
    ap.add_argument("--font", default="Heebo-Bold")
    ap.add_argument("--sleep", action="store_true", help="render the night image")
    ap.add_argument("--no-weather", action="store_true")
    ap.add_argument("--out", default="panel_preview.png")
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--port", type=int, default=8800)
    a = ap.parse_args()
    if a.serve:
        serve(a)
    else:
        render(a).save(a.out)
        print("wrote", a.out)
