import io

import requests
from PIL import Image

WIDTH, HEIGHT = 256, 128
SZ = WIDTH * HEIGHT // 8


def convert_to_frame(source, invert=False, threshold=150, dither=True):
    """Картинка (путь или URL) -> raw 4096 байт (row-lsb, 1 bpp)."""
    if isinstance(source, str) and source.startswith(("http://", "https://")):
        r = requests.get(source, timeout=15)
        r.raise_for_status()
        img = Image.open(io.BytesIO(r.content))
    else:
        img = Image.open(source)

    img = img.convert("L")
    img.thumbnail((WIDTH, HEIGHT), Image.LANCZOS)

    canvas = Image.new("L", (WIDTH, HEIGHT), 255)
    canvas.paste(img, ((WIDTH - img.width) // 2,
                       (HEIGHT - img.height) // 2))

    if dither:
        bw = canvas.convert("1", dither=Image.FLOYDSTEINBERG)
    else:
        bw = canvas.point(lambda p: 255 if p >= threshold else 0, "1")

    buf = bytearray([0xFF] * SZ)
    px = bw.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if px[x, y] == 0:                       # чёрный пиксель
                buf[y * 32 + (x // 8)] &= ~(1 << (x % 8))

    frame = bytes(buf)
    if invert:
        frame = bytes(b ^ 0xFF for b in frame)
    return frame


# ============ точка входа python_script.exec ============
ip = data.get("ip")
img = data.get("img")
invert = bool(data.get("invert", False))
dither = bool(data.get("dither", True))
threshold = int(data.get("threshold", 150))

logger.info(f"OTA5901: ip={ip}, img={img}, invert={invert}, dither={dither}")

try:
    frame = convert_to_frame(img, invert=invert,
                             threshold=threshold, dither=dither)
    logger.info(f"OTA5901: frame ready, {len(frame)} bytes")

    url = f"http://{ip}/ota5901/frame"
    r = requests.post(
        url,
        data=frame,
        headers={"Content-Type": "application/octet-stream"},
        timeout=20,
    )
    logger.info(f"OTA5901: POST {url} -> {r.status_code} {r.text}")

    hass.states.set(
        "sensor.ota5901_status",
        f"ok ({r.status_code})",
        {"friendly_name": "OTA5901 Status"},
    )
except Exception as e:
    logger.error(f"OTA5901: failed - {e}")
    hass.states.set(
        "sensor.ota5901_status",
        f"error: {e}",
        {"friendly_name": "OTA5901 Status"},
    )