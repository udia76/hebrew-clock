"""Synchronous image-generation service. Runs in a thread-pool worker."""
import datetime
import io
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from loguru import logger

from app.core.config import settings

VALID_FONTS = {
    "DavidLibre-Bold", "FrankRuhlLibre-Bold", "FrankRuhlLibre",
    "Heebo-Bold", "NotoSansHebrew-Bold",
}
DEFAULT_FONT = "Heebo-Bold"

# ── Hebrew time tables ────────────────────────────────

HOURS = [
    "אַחַת", "שְׁתַּיִם", "שָׁלוֹשׁ", "אַרְבַּע", "חָמֵשׁ", "שֵׁשׁ",
    "שֶׁבַע", "שְׁמוֹנֶה", "תֵּשַׁע", "עֶשֶׂר", "אַחַת עֶשְׂרֵה", "שְׁתֵּים עֶשְׂרֵה"
]
MINUTE_PREFIX = [
    "", "וְדַקָּה אַחַת", "וּשְׁתֵּי דַקּוֹת", "וְשָׁלוֹשׁ דַקּוֹת",
    "וְאַרְבַּע דַקּוֹת", "וְחָמֵשׁ דַקּוֹת", "וְשֵׁשׁ דַקּוֹת", "וְשֶׁבַע דַקּוֹת",
    "וּשְׁמוֹנֶה דַקּוֹת", "וְתֵשַׁע דַקּוֹת", "וְעֶשֶׂר דַקּוֹת",
    "וְאַחַת עֶשְׂרֵה דַּקּוֹת", "וּשְׁתֵּים עֶשְׂרֵה דַּקּוֹת",
    "וּשְׁלוֹשׁ עֶשְׂרֵה דַּקּוֹת", "וְאַרְבַּע עֶשְׂרֵה דַּקּוֹת",
    "וָרֶבַע", "וְשֵׁשׁ עֶשְׂרֵה דַּקּוֹת", "וּשְׁבַע עֶשְׂרֵה דַּקּוֹת",
    "וּשְׁמוֹנֶה עֶשְׂרֵה דַּקּוֹת", "וּתְשַׁע עֶשְׂרֵה דַּקּוֹת",
    "וְעֶשְׂרִים דַקּוֹת", "וְעֶשְׂרִים וְאַחַת", "וְעֶשְׂרִים וּשְׁתַּיִם",
    "וְעֶשְׂרִים וְשָׁלוֹשׁ", "וְעֶשְׂרִים וְאַרְבַּע", "וְעֶשְׂרִים וְחָמֵשׁ",
    "וְעֶשְׂרִים וְשֵׁשׁ", "וְעֶשְׂרִים וְשֶׁבַע", "וְעֶשְׂרִים וּשְׁמוֹנֶה",
    "וְעֶשְׂרִים וְתֵשַׁע", "וּשְׁלוֹשִׁים", "וּשְׁלוֹשִׁים וְאַחַת",
    "וּשְׁלוֹשִׁים וּשְׁתַּיִם", "וּשְׁלוֹשִׁים וְשָׁלוֹשׁ", "וּשְׁלוֹשִׁים וְאַרְבַּע",
    "וּשְׁלוֹשִׁים וְחָמֵשׁ", "וּשְׁלוֹשִׁים וְשֵׁשׁ", "וּשְׁלוֹשִׁים וְשֶׁבַע",
    "וּשְׁלוֹשִׁים וּשְׁמוֹנֶה", "וּשְׁלוֹשִׁים וְתֵשַׁע", "וְאַרְבָּעִים",
    "וְאַרְבָּעִים וְאַחַת", "וְאַרְבָּעִים וּשְׁתַּיִם", "וְאַרְבָּעִים וְשָׁלוֹשׁ",
    "וְאַרְבָּעִים וְאַרְבַּע", "וְאַרְבָּעִים וְחָמֵשׁ", "וְאַרְבָּעִים וְשֵׁשׁ",
    "וְאַרְבָּעִים וְשֶׁבַע", "וְאַרְבָּעִים וּשְׁמוֹנֶה", "וְאַרְבָּעִים וְתֵשַׁע",
    "וַחֲמִשִּׁים", "וַחֲמִשִּׁים וְאַחַת", "וַחֲמִשִּׁים וּשְׁתַּיִם",
    "וַחֲמִשִּׁים וְשָׁלוֹשׁ", "וַחֲמִשִּׁים וְאַרְבַּע", "וַחֲמִשִּׁים וְחָמֵשׁ",
    "וַחֲמִשִּׁים וְשֵׁשׁ", "וַחֲמִשִּׁים וְשֶׁבַע", "וַחֲמִשִּׁים וּשְׁמוֹנֶה",
    "וַחֲמִשִּׁים וְתֵשַׁע",
]

PERIOD_WORDS = {
    "בַּבֹּקֶר", "בַּצָּהֳרַיִם", "אַחַר הַצָּהֳרַיִם",
    "בָּעֶרֶב", "בַּלַּיְלָה", "לִפְנוֹת בֹּקֶר",
}

MONTHS_HE = [
    "בְּיָנוּאָר", "בְּפֶבְּרוּאָר", "בְּמָרְץ", "בְּאַפְּרִיל",
    "בְּמַאי", "בְּיוּנִי", "בְּיוּלִי", "בְּאוֹגוּסְט",
    "בְּסֶפְּטֶמְבֶּר", "בְּאוֹקְטוֹבֶּר", "בְּנוֹבֶמְבֶּר", "בְּדֶצֶמְבֶּר",
]
DAYS_HE = [
    "יוֹם שֵׁנִי", "יוֹם שְׁלִישִׁי", "יוֹם רְבִיעִי",
    "יוֹם חֲמִישִׁי", "יוֹם שִׁישִּׁי", "שַׁבָּת", "יוֹם רִאשׁוֹן",
]

# ── Helpers ───────────────────────────────────────────

def get_israel_time() -> datetime.datetime:
    utc = datetime.datetime.utcnow()
    local = utc + datetime.timedelta(hours=3 if 3 <= utc.month <= 10 else 2)
    return local + datetime.timedelta(seconds=settings.display_lag)


def get_font(size: int, font_name: str = DEFAULT_FONT) -> ImageFont.FreeTypeFont:
    name = font_name if font_name in VALID_FONTS else DEFAULT_FONT
    path = settings.font_dir / f"{name}.ttf"
    if path.exists():
        try:
            return ImageFont.truetype(str(path), size)
        except Exception:
            pass
    for fallback in ("NotoSansHebrew-Bold", "FrankRuhlLibre"):
        fb = settings.font_dir / f"{fallback}.ttf"
        if fb.exists():
            try:
                return ImageFont.truetype(str(fb), size)
            except Exception:
                pass
    return ImageFont.load_default()


def _png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.convert("1", dither=Image.Dither.NONE).save(buf, format="PNG", optimize=True)
    buf.seek(0)
    return buf.read()


def _strip_niqqud(text: str) -> str:
    return "".join(ch for ch in text if not "\u05b0" <= ch <= "\u05c7")


def _png_bytes_3c(black: Image.Image, red_mask: Image.Image) -> bytes:
    """Combine an L image (0 = ink) and a red mask (255 = red) into a
    4-colour paletted PNG: white, black, red."""
    rgb = black.convert("RGB")
    rgb.paste((255, 0, 0), mask=red_mask.convert("L").point(lambda p: 255 if p > 127 else 0))
    pal = Image.new("P", (1, 1))
    pal.putpalette([255, 255, 255, 0, 0, 0, 255, 0, 0] + [0] * (256 * 3 - 9))
    q = rgb.quantize(palette=pal, dither=Image.Dither.NONE)
    buf = io.BytesIO()
    q.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _get_time_period(h: int) -> str:
    if 6  <= h < 12: return "בַּבֹּקֶר"
    if 12 <= h < 16: return "בַּצָּהֳרַיִם"
    if 16 <= h < 18: return "אַחַר הַצָּהֳרַיִם"
    if 18 <= h < 21: return "בָּעֶרֶב"
    if 21 <= h < 24: return "בַּלַּיְלָה"
    if 0  <= h < 3:  return "בַּלַּיְלָה"
    return "לִפְנוֹת בֹּקֶר"


def _get_time_lines(h24: int, m: int) -> list[str]:
    h12 = h24 % 12 or 12
    period = _get_time_period(h24)
    mp = MINUTE_PREFIX[m]
    hp = HOURS[h12 - 1]
    if len(hp + mp) > 25:
        return [hp, mp, period]
    return [hp + " " + mp, period]

# ── Drawing ───────────────────────────────────────────

def _draw_weather_icon(draw: ImageDraw.Draw, cx: int, cy: int,
                       icon_key: str, size: int = 38) -> None:
    s = size

    def cloud(ox: int = 0, oy: int = 0, scale: float = 1.0) -> None:
        w, h = int(s * 1.4 * scale), int(s * 0.7 * scale)
        pts = []
        for a in range(180, 361, 8):
            pts.append((ox + cx + int(w / 2 * math.cos(math.radians(a))),
                        oy + cy + int(h / 2 * math.sin(math.radians(a)))))
        for centre, rx, dy in [
            (int(w * 0.25),  int(h * 0.6  * scale), int(h * 0.2)),
            (0,              int(h * 0.75 * scale), int(h * 0.3)),
            (-int(w * 0.25), int(h * 0.55 * scale), int(h * 0.1)),
        ]:
            for a in range(0, 181, 8):
                pts.append((ox + cx + centre + int(rx * math.cos(math.radians(a))),
                            oy + cy - dy     + int(rx * math.sin(math.radians(a)))))
        draw.polygon(pts, fill=255, outline=0)

    if icon_key == "sun":
        r = s // 2
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255, outline=0, width=3)
        for a in range(0, 360, 45):
            rad = math.radians(a)
            draw.line([cx + (r + 4)  * math.cos(rad), cy + (r + 4)  * math.sin(rad),
                       cx + (r + 13) * math.cos(rad), cy + (r + 13) * math.sin(rad)],
                      fill=0, width=3)
    elif icon_key == "sun_cloud":
        sr = s // 3
        scx, scy = cx - s // 3, cy - s // 4
        draw.ellipse([scx - sr, scy - sr, scx + sr, scy + sr], fill=255, outline=0, width=2)
        for a in range(0, 360, 60):
            rad = math.radians(a)
            draw.line([scx + (sr + 3) * math.cos(rad), scy + (sr + 3) * math.sin(rad),
                       scx + (sr + 9) * math.cos(rad), scy + (sr + 9) * math.sin(rad)],
                      fill=0, width=2)
        cloud(s // 5, s // 5, 0.85)
    elif icon_key == "cloud":
        cloud()
    elif icon_key == "cloud_rain":
        cloud(0, -s // 5, 0.9)
        for ox in (-s // 3, -s // 8, s // 8, s // 3):
            draw.line([cx + ox, cy + s // 4, cx + ox - 4, cy + s // 2 + 4], fill=0, width=2)
    elif icon_key == "cloud_snow":
        cloud(0, -s // 5, 0.9)
        for ox in (-s // 3, -s // 8, s // 8, s // 3):
            x, y = cx + ox, cy + s // 3
            for a in (0, 60, 120):
                rad = math.radians(a)
                draw.line([x - 6 * math.cos(rad), y - 6 * math.sin(rad),
                           x + 6 * math.cos(rad), y + 6 * math.sin(rad)],
                          fill=0, width=2)
    elif icon_key == "thunder":
        cloud(0, -s // 4, 0.9)
        pts = [(cx + 4, cy + s // 6), (cx - 6, cy + s // 2),
               (cx + 2, cy + s // 2), (cx - 8, cy + s)]
        draw.line(pts, fill=0, width=3)


def _draw_analog_clock(draw: ImageDraw.Draw, cx: int, cy: int, r: int,
                       h24: int, m: int, font_name: str) -> None:
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=0, width=3)
    for i in range(12):
        angle = math.radians(i * 30 - 90)
        if i % 3 == 0:
            draw.line([cx + (r - 4)  * math.cos(angle), cy + (r - 4)  * math.sin(angle),
                       cx + (r - 12) * math.cos(angle), cy + (r - 12) * math.sin(angle)],
                      fill=0, width=3)
        else:
            draw.line([cx + (r - 4) * math.cos(angle), cy + (r - 4) * math.sin(angle),
                       cx + (r - 9) * math.cos(angle), cy + (r - 9) * math.sin(angle)],
                      fill=0, width=2)
    num_font = get_font(max(12, r // 4), font_name)
    for num, deg in ((12, -90), (3, 0), (6, 90), (9, 180)):
        angle = math.radians(deg)
        draw.text((cx + (r - 18) * math.cos(angle), cy + (r - 18) * math.sin(angle)),
                  str(num), font=num_font, fill=0, anchor="mm")
    h12 = h24 % 12
    hour_angle = math.radians((h12 + m / 60) * 30 - 90)
    draw.line([cx, cy, cx + (r * 0.55) * math.cos(hour_angle),
               cy + (r * 0.55) * math.sin(hour_angle)], fill=0, width=4)
    min_angle = math.radians(m * 6 - 90)
    draw.line([cx, cy, cx + (r * 0.75) * math.cos(min_angle),
               cy + (r * 0.75) * math.sin(min_angle)], fill=0, width=2)
    draw.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=0)

# Panel resolution (HINK-E075A07-A0 / GDEH075Z90: 880x528)
PANEL_W, PANEL_H = 880, 528

# ── Image generators ──────────────────────────────────

def _generate_night_image(font_name: str) -> bytes:
    W, H = PANEL_W, PANEL_H
    img = Image.new("L", (W, H), color=0)
    draw = ImageDraw.Draw(img)

    rng = random.Random(42)
    for _ in range(60):
        x = rng.randint(20, W - 20)
        y = rng.randint(20, H - 20)
        size = rng.choice([1, 1, 2, 2, 3])
        draw.ellipse([x - size, y - size, x + size, y + size], fill=255)

    mx, my, mr = 100, 90, 55
    draw.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=255)
    draw.ellipse([mx - mr + 16, my - mr - 12, mx + mr + 16, my - mr - 12 + mr * 2], fill=0)

    sleeping_path = settings.font_dir / "sleeping.png"
    if sleeping_path.exists():
        try:
            sleeping = Image.open(sleeping_path).convert("L")
            mask = sleeping.point(lambda p: 255 if p < 128 else 0)
            white_lines = Image.new("L", sleeping.size, 255)
            black_bg   = Image.new("L", sleeping.size, 0)
            result = Image.composite(white_lines, black_bg, mask)
            sw, sh = 380, 280
            result = result.resize((sw, sh), Image.LANCZOS)
            mask_r = mask.resize((sw, sh), Image.LANCZOS)
            img.paste(result, (W - sw - 20, (H - sh) // 2), mask=mask_r)
        except Exception as exc:
            logger.warning("sleeping image error: {}", exc)

    text_cx = (W - 380 - 40) // 2
    draw.text((text_cx, H // 2 - 30), "זְמַן לִישׁוֹן",
              font=get_font(72, font_name), fill=255, anchor="mm")
    draw.text((text_cx, H // 2 + 55), "לַיְלָה טוֹב",
              font=get_font(44, font_name), fill=180, anchor="mm")
    return _png_bytes(img)


def _generate_quiet_image(font_name: str) -> bytes:
    W, H = PANEL_W, PANEL_H
    img = Image.new("L", (W, H), color=255)
    draw = ImageDraw.Draw(img)
    PAD1, PAD2 = 8, 16
    draw.rectangle([PAD1, PAD1, W - PAD1, H - PAD1], outline=0, width=3)
    draw.rectangle([PAD2, PAD2, W - PAD2, H - PAD2], outline=0, width=1)
    draw.text((W // 2, H // 2 - 50), "לֹא לְהָעִיר אַף אֶחָד!",
              font=get_font(72, font_name), fill=0, anchor="mm")
    draw.text((W // 2 - 60, H // 2 + 30), "z", font=get_font(72, font_name), fill=0, anchor="mm")
    draw.text((W // 2,      H // 2 + 20), "z", font=get_font(55, font_name), fill=0, anchor="mm")
    draw.text((W // 2 + 50, H // 2 + 10), "z", font=get_font(38, font_name), fill=0, anchor="mm")
    return _png_bytes(img)


def _generate_minimal_image(font_name: str, now: datetime.datetime,
                            weather: dict | None, jewish_date: str | None,
                            calendar: str = "gregorian") -> bytes:
    """Minimalist black / white / red layout."""
    W, H = PANEL_W, PANEL_H
    img = Image.new("L", (W, H), color=255)
    red = Image.new("L", (W, H), color=0)
    d, dr = ImageDraw.Draw(img), ImageDraw.Draw(red)

    M = 44                                   # inner margin
    d.rounded_rectangle([12, 12, W - 12, H - 12], radius=20, outline=0, width=2)

    # header: main date (right) and digital time (left)
    greg_txt = _strip_niqqud(
        f"{DAYS_HE[now.weekday()]}, {now.day} {MONTHS_HE[now.month - 1]} {now.year}")
    heb_txt = _strip_niqqud(jewish_date.replace("\n", " ")) if jewish_date else ""
    if calendar == "jewish" and heb_txt:
        date_txt, second_txt = heb_txt, f"{now.day} {_strip_niqqud(MONTHS_HE[now.month - 1])} {now.year}"
    else:
        date_txt, second_txt = greg_txt, heb_txt
    head_font = get_font(30, font_name)
    d.text((W - M, 62), date_txt, font=head_font, fill=0, anchor="rm")
    d.text((M, 62), f"{now.hour:02d}:{now.minute:02d}", font=head_font, fill=0, anchor="lm")

    # centre: the time as words
    lines = _get_time_lines(now.hour, now.minute)
    period = next((l for l in lines if l in PERIOD_WORDS), "")
    words = [l for l in lines if l not in PERIOD_WORDS]
    prefix = "הַשָּׁעָה"
    one_line = " ".join([prefix] + words + ([period] if period else []))
    max_w = W - 2 * M - 20

    def fit(text: str, start: int, minimum: int = 40):
        size = start
        while True:
            f = get_font(size, font_name)
            bb = d.textbbox((0, 0), text, font=f)
            if bb[2] - bb[0] <= max_w or size <= minimum:
                return f
            size -= 4

    cy = H // 2 - 10
    f1 = fit(one_line, 72, 56)
    bb = d.textbbox((0, 0), one_line, font=f1)
    if bb[2] - bb[0] <= max_w:
        rows = [one_line]
    else:
        rows = [" ".join([prefix, words[0]]), " ".join(words[1:] + ([period] if period else []))]
        rows = [r for r in rows if r.strip()]
    fr = [fit(r, 72, 40) for r in rows]
    line_h = 96
    top = cy - (len(rows) - 1) * line_h // 2
    for i, (r, f) in enumerate(zip(rows, fr)):
        d.text((W // 2, top + i * line_h), r, font=f, fill=0, anchor="mm")
    ul_y = top + (len(rows) - 1) * line_h + 78
    dr.rectangle([W // 2 - 46, ul_y, W // 2 + 46, ul_y + 5], fill=255)

    # divider + footer: second date (right cell) | weather (left cell)
    sep_y = H - 112
    d.line([(M, sep_y), (W - M, sep_y)], fill=0, width=1)
    foot_cy = H - 64
    cell_l, cell_r = (M + W // 2) // 2, (W // 2 + W - M) // 2
    if weather and second_txt:
        d.line([(W // 2, sep_y + 22), (W // 2, H - 26)], fill=0, width=1)
    elif weather:
        cell_l = W // 2
    elif second_txt:
        cell_r = W // 2

    if second_txt:
        df = get_font(34, font_name)
        while d.textbbox((0, 0), second_txt, font=df)[2] > (W - 2 * M) // 2 - 20 and df.size > 20:
            df = get_font(df.size - 2, font_name)
        d.text((cell_r, foot_cy), second_txt, font=df, fill=0, anchor="mm")

    if weather:
        desc = _strip_niqqud(weather.get("desc", ""))
        txt = f"{weather['temp']}°  {desc}".strip()
        wf = get_font(34, font_name)
        tb = d.textbbox((0, 0), txt, font=wf)
        tw = tb[2] - tb[0]
        text_x = cell_l - 36
        d.text((text_x, foot_cy), txt, font=wf, fill=0, anchor="mm")
        _draw_weather_icon(d, text_x + tw // 2 + 52, foot_cy, weather.get("icon_key", "cloud"), size=40)

    return _png_bytes_3c(img, red)


def generate_clock_image(
    font_name:   str        = DEFAULT_FONT,
    sleep_time:  bool       = False,
    weather:     dict | None = None,
    jewish_date: str | None  = None,
    calendar:    str         = "gregorian",
) -> bytes:
    fn = font_name if font_name in VALID_FONTS else DEFAULT_FONT

    if sleep_time:
        return _generate_night_image(fn)

    now  = get_israel_time()
    h24, m = now.hour, now.minute

    if h24 == 6 or (h24 == 7 and m < 30):
        return _generate_quiet_image(fn)

    return _generate_minimal_image(fn, now, weather, jewish_date, calendar)


def log_available_fonts() -> None:
    found = [f for f in VALID_FONTS
             if (settings.font_dir / f"{f}.ttf").exists()]
    if found:
        logger.info("available fonts: {}", ", ".join(sorted(found)))
    else:
        logger.warning("no Hebrew font files found in {}", settings.font_dir)
