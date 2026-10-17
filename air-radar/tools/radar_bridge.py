"""Optional USB aircraft feed for Air Radar; location is read from device setup."""
import argparse
import json
import logging
from logging.handlers import RotatingFileHandler
import math
from pathlib import Path
import queue
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

import serial
from routes import RouteCache

LAT, LON = 0.0, 0.0  # Neutral defaults for pure feed-parser tests; live use requires setup.
SPANS = (25, 50, 100)
MAX_AIRCRAFT = 64
MAX_AGE = 30
LOG = logging.getLogger("radar")


class FeedFailure(Exception):
    def __init__(self, message, retry=10):
        super().__init__(message)
        self.retry = retry


def local_km(lat, lon, center=None):
    latitude, longitude = (LAT, LON) if center is None else center
    north = math.radians(lat - latitude) * 6371.0088
    east = math.radians(math.remainder(lon - longitude, 360)) * 6371.0088 * math.cos(math.radians((lat + latitude) / 2))
    return east, north


def radius_nm(span):
    # The endpoint takes nautical miles. Cover all four corners of the square.
    return math.ceil((span / 2) * math.sqrt(2) / 1.852) + 1


def finite(value, minimum, maximum):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and minimum <= value <= maximum)


def safe_text(value, length):
    return re.sub(r"[^A-Za-z0-9-]", "", str(value or ""))[:length]


def normalize(raw, span, sequence, received_at=None, center=None):
    """Discard ground traffic, unlocated aircraft and stale positions."""
    received_at = time.time() if received_at is None else received_at
    stamp = raw.get("now")
    if not finite(stamp, 1_000_000_000_000, 9_999_999_999_999):
        raise FeedFailure("Nema vremenske oznake u podacima")
    feed_age = received_at - stamp / 1000
    if feed_age > MAX_AGE or feed_age < -10:
        raise FeedFailure("Podaci su stari ili vreme na racunaru nije ispravno")
    if not isinstance(raw.get("ac"), list):
        raise FeedFailure("Neispravan odgovor sa podacima o avionima")
    aircraft, seen = [], set()
    for item in raw["ac"]:
        if not isinstance(item, dict):
            continue
        lat, lon, age = item.get("lat"), item.get("lon"), item.get("seen_pos")
        identifier = safe_text(item.get("hex"), 8)
        if (not identifier or identifier in seen or item.get("alt_baro") == "ground"
                or not finite(lat, -90, 90) or not finite(lon, -180, 180)
                or not finite(age, 0, MAX_AGE)):
            continue
        age += max(0, feed_age)
        if age >= MAX_AGE:
            continue
        east, north = local_km(lat, lon, center)
        if abs(east) > span / 2 or abs(north) > span / 2:
            continue
        seen.add(identifier)
        gs, track = item.get("gs"), item.get("track")
        aircraft.append({
            "hex": identifier,
            "flight": safe_text(item.get("flight") or item.get("r"), 12),
            "callsign": safe_text(item.get("flight"), 12).upper(),
            "model": safe_text(item.get("t"), 8),
            "lat": round(lat, 6), "lon": round(lon, 6), "age": round(age, 2),
            "speed": round(gs * 1.852, 1) if finite(gs, 0, 1600) else None,
            "track": round(track, 2) if finite(track, 0, 359.9999) else None,
        })
    aircraft.sort(key=lambda a: math.hypot(*local_km(a["lat"], a["lon"], center)))
    return {"type": "aircraft", "v": 1, "lat": (center or (LAT,LON))[0], "lon": (center or (LAT,LON))[1],
            "now": int(stamp), "seq": sequence, "ac": aircraft[:MAX_AIRCRAFT]}


def fetch(span, center=None):
    if center is None:
        raise FeedFailure("Choose a location in radar setup first")
    latitude, longitude = center
    url = f"https://api.adsb.lol/v2/point/{latitude:.6f}/{longitude:.6f}/{radius_nm(span)}"
    request = urllib.request.Request(url, headers={
        "User-Agent": "AirRadar/3.0 (10-second polling)",
        "Accept": "application/json", "Cache-Control": "no-cache"})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            body = response.read(2 * 1024 * 1024 + 1)
            if len(body) > 2 * 1024 * 1024:
                raise FeedFailure("Odgovor sa podacima je prevelik", 60)
            return json.loads(body)
    except urllib.error.HTTPError as error:
        retry = 300 if error.code in (401, 403) else 60
        if error.code == 429:
            try:
                retry = max(10, min(600, int(error.headers.get("Retry-After", "60"))))
            except ValueError:
                pass
        raise FeedFailure(f"ADS-B.lol HTTP {error.code}", retry) from error
    except (urllib.error.URLError, TimeoutError, ValueError) as error:
        raise FeedFailure(f"Podaci o avionima nisu dostupni: {error}") from error


def save_bmp(path, raw, width, height):
    """Export the exact RGB565 pixels flushed by LVGL to the physical display."""
    stride = (width * 3 + 3) & ~3
    size = stride * height
    header = struct.pack("<2sIHHI", b"BM", 54 + size, 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, size, 2835, 2835, 0, 0)
    output = bytearray(header)
    for y in range(height - 1, -1, -1):
        for x in range(width):
            pixel = struct.unpack_from("<H", raw, (y * width + x) * 2)[0]
            r, g, b = (pixel >> 11) & 31, (pixel >> 5) & 63, pixel & 31
            output.extend((b * 255 // 31, g * 255 // 63, r * 255 // 31))
        output.extend(b"\x00" * (stride - width * 3))
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(output)


class Board:
    def __init__(self, port, span, capture=None):
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=3)
        self.serial.port = port
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.span = span
        self.stop = threading.Event()
        self.events = queue.Queue(maxsize=100)
        self.write_lock = threading.Lock()
        self.capturing = threading.Event()
        self.capture_path = Path(capture) if capture else Path(__file__).resolve().parents[1] / "radar-screen.bmp"
        self.thread = threading.Thread(target=self.reader, daemon=True)
        self.thread.start()

    def send(self, packet):
        encoded = json.dumps(packet, separators=(",", ":"), allow_nan=False).encode("ascii") + b"\n"
        if len(encoded) >= 32768:
            raise ValueError("USB poruka je prevelika za plocu")
        with self.write_lock:
            self.serial.write(encoded)

    def key(self, key):
        if key == "p":
            self.capturing.set()
        self.send({"type": "command", "name": "key", "value": key})

    def reader(self):
        try:
            while not self.stop.is_set():
                line = self.serial.readline(4096)
                if not line:
                    continue
                try:
                    event = json.loads(line)
                except (ValueError, UnicodeDecodeError):
                    pass  # Unstructured device output may contain local network details.
                    continue
                kind = event.get("event")
                if kind == "screenshot":
                    self.capturing.set()
                    if event.get("bytes") != 115200 or event.get("format") != "rgb565le":
                        raise ValueError("Neispravan format snimka ekrana")
                    raw = bytearray()
                    deadline = time.monotonic() + 25
                    while len(raw) < 115200 and time.monotonic() < deadline and not self.stop.is_set():
                        raw.extend(self.serial.read(min(4096, 115200 - len(raw))))
                    if len(raw) != 115200:
                        raise ValueError("Snimak ekrana nije potpun")
                    save_bmp(self.capture_path, raw, 240, 240)
                    LOG.info("Snimak ekrana je sacuvan: %s", self.capture_path)
                    self.capturing.clear()
                    event = {"event": "captured", "path": str(self.capture_path)}
                elif kind == "settings":
                    if event.get("span") in SPANS:
                        self.span = event["span"]
                    LOG.info("Prikaz: %s x %s km; osvetljenje %s; rotacija %s",
                             self.span, self.span, event.get("brightness"), event.get("rotation"))
                elif kind == "feed":
                    LOG.info("PODACI: %s aviona; izabrani %s na %.1f km (osvezavanje %s)",
                             event.get("count"), event.get("nearest") or "--",
                             event.get("distance", 0), event.get("seq"))
                elif kind in ("rejected", "feed_error", "error"):
                    LOG.warning("Ploca: %s", event)
                else:
                    pass  # Do not log setup responses, network identifiers or access keys.
                try:
                    self.events.put_nowait(event)
                except queue.Full:
                    self.events.get_nowait()
                    self.events.put_nowait(event)
        except (serial.SerialException, OSError, ValueError) as error:
            if not self.stop.is_set():
                LOG.error("USB veza je prekinuta: %s", error)
                self.stop.set()

    def wait_for(self, kind, timeout=6, seq=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and not self.stop.is_set():
            try:
                event = self.events.get(timeout=0.2)
            except queue.Empty:
                continue
            if event.get("event") == kind and (seq is None or event.get("seq") == seq):
                return event
        raise RuntimeError(f"Ploca nije potvrdila poruku: {kind}")

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        self.serial.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None, help="USB port ploce")
    parser.add_argument("--span", type=int, choices=SPANS, default=50, help="Sirina i visina prikaza u km")
    parser.add_argument("--interval", type=float, default=10, help="Razmak osvezavanja u sekundama")
    parser.add_argument("--once", action="store_true", help="Jedno provereno osvezavanje, pa izlaz")
    parser.add_argument("--capture", help="Sacuvaj stvarni LCD prikaz kao BMP posle prvog osvezavanja")
    parser.add_argument("--verbose", action="store_true", help="Detaljan zapis rada")
    parser.add_argument("--log", default=str(Path(__file__).resolve().parents[1] / "radar.log"), help="Datoteka sa zapisom rada")
    args = parser.parse_args()
    if args.interval < 10:
        parser.error("Razmak osvezavanja mora biti bar 10 sekundi")
    LOG.setLevel(logging.DEBUG if args.verbose else logging.INFO)
    formatter = logging.Formatter("%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S")
    handlers = [RotatingFileHandler(args.log, maxBytes=1048576, backupCount=2, encoding="utf-8")]
    if sys.stderr.isatty() or args.verbose:
        handlers.append(logging.StreamHandler())
    for handler in handlers:
        handler.setFormatter(formatter)
        LOG.addHandler(handler)
    board = None
    routes = None
    try:
        from wifi_setup import choose_port
        board = Board(choose_port(args.port), args.span, args.capture)
        board.send({"type": "command", "name": "hello"})
        board.wait_for("ready")
        board.send({"type": "command", "name": "zoom", "value": args.span})
        board.wait_for("settings")
        board.send({"type":"command","name":"wifi_info"})
        info=board.wait_for("network")
        if not info.get("configured") or info.get("mode")!="usb":
            raise RuntimeError("Open radar setup and choose USB from computer first")
        center=(info["latitude"],info["longitude"])
        routes = RouteCache()
        LOG.info("USB aircraft feed running; settings stay on the device.")
        LOG.info("Tasteri: + / - opseg, n sledeci avion, a najblizi, [ / ] osvetljenje, p snimak, q izlaz.")
        sequence = 0
        failures = 0
        next_fetch = 0.0
        captured = False
        if sys.platform == "win32":
            import msvcrt
        while not board.stop.is_set():
            if sys.platform == "win32" and sys.stdin.isatty() and msvcrt.kbhit():
                key = msvcrt.getwch()
                if key in ("q", "\x03"):
                    break
                if key in "+-na[]rshp":
                    board.key(key)
            now = time.monotonic()
            if now >= next_fetch and not board.capturing.is_set():
                try:
                    sequence += 1
                    packet = routes.enrich(normalize(fetch(board.span, center), board.span, sequence, center=center))
                    board.send(packet)
                    board.wait_for("feed", seq=sequence)
                    failures = 0
                    next_fetch = time.monotonic() + args.interval
                    if args.capture and not captured:
                        time.sleep(0.3)
                        board.key("p")
                        board.wait_for("captured", timeout=30)
                        captured = True
                    if args.once:
                        break
                except FeedFailure as error:
                    failures += 1
                    LOG.warning("%s", error)
                    board.send({"type": "error"})
                    if args.once:
                        raise
                    next_fetch = time.monotonic() + max(error.retry, min(120, args.interval * 2 ** min(failures - 1, 4)))
            time.sleep(0.05)
        if board.stop.is_set():
            raise RuntimeError("Ploca nije povezana; proveri USB i ponovo pokreni radar")
        return 0
    except KeyboardInterrupt:
        return 0
    except (serial.SerialException, OSError, RuntimeError, ValueError, FeedFailure) as error:
        LOG.error("%s", error)
        LOG.error("COM4 moze da koristi samo jedan program. Zatvori PlatformIO Serial Monitor pre pokretanja radara.")
        return 1
    finally:
        if board:
            board.close()
        if routes:
            routes.close()


if __name__ == "__main__":
    raise SystemExit(main())
