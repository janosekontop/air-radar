"""Procene ruta po pozivnom znaku; upiti rade odvojeno od podataka o poziciji."""
import json
import logging
import math
from pathlib import Path
import queue
import re
import threading
import time
import unicodedata
import urllib.error
import urllib.request

LOG = logging.getLogger('radar')

# English city names for common airports; identifiers remain real IATA/ICAO codes.
AIRPORT_CITIES = {
    'BEG': 'Belgrade', 'LYBE': 'Belgrade', 'OTP': 'Bucharest', 'BBU': 'Bucharest',
    'IST': 'Istanbul', 'SAW': 'Istanbul', 'ADB': 'Izmir', 'ESB': 'Ankara',
    'AYT': 'Antalya', 'LHR': 'London', 'LGW': 'London', 'STN': 'London',
    'LTN': 'London', 'LCY': 'London', 'NUE': 'Nuremberg', 'MUC': 'Munich',
    'VIE': 'Vienna', 'ZRH': 'Zurich', 'CDG': 'Paris', 'ORY': 'Paris',
    'FRA': 'Frankfurt', 'DUS': 'Dusseldorf', 'CGN': 'Cologne', 'BER': 'Berlin',
    'FCO': 'Rome', 'CIA': 'Rome', 'MXP': 'Milan', 'LIN': 'Milan',
    'ATH': 'Athens', 'SKG': 'Thessaloniki', 'BUD': 'Budapest', 'SOF': 'Sofia',
    'SJJ': 'Sarajevo', 'ZAG': 'Zagreb', 'LJU': 'Ljubljana', 'SKP': 'Skopje',
    'TGD': 'Podgorica', 'TIV': 'Tivat', 'INI': 'Nis', 'TLL': 'Tallinn',
    'PRG': 'Prague', 'WAW': 'Warsaw', 'CPH': 'Copenhagen', 'AMS': 'Amsterdam',
    'BRU': 'Brussels', 'ARN': 'Stockholm', 'GOT': 'Gothenburg', 'OSL': 'Oslo',
    'MAD': 'Madrid', 'BCN': 'Barcelona', 'LIS': 'Lisbon', 'LED': 'Saint Petersburg',
    'SVO': 'Moscow', 'DME': 'Moscow', 'VKO': 'Moscow', 'DXB': 'Dubai',
    'AUH': 'Abu Dhabi', 'DOH': 'Doha', 'JFK': 'New York', 'EWR': 'New York',
    'ORD': 'Chicago', 'YYZ': 'Toronto', 'PEK': 'Beijing', 'PKX': 'Beijing',
}


def airport_city(airport):
    code = airport_code(airport)
    if code in AIRPORT_CITIES:
        return AIRPORT_CITIES[code]
    if not isinstance(airport, dict) or not isinstance(airport.get('location'), str):
        return ''
    # The compact built-in font is ASCII. Keep full names, strip unsupported marks.
    text = airport['location'].replace('đ', 'dj').replace('Đ', 'Dj')
    text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode()
    text = ' '.join(text.split())
    text = re.sub(r'[^A-Za-z0-9 .()-]', '', text)
    return ' '.join(text.split())[:47]


def airport_code(airport):
    if not isinstance(airport, dict):
        return ''
    iata, icao = airport.get('iata'), airport.get('icao')
    if isinstance(iata, str) and re.fullmatch(r'[A-Z]{3}', iata):
        return iata
    if isinstance(icao, str) and re.fullmatch(r'[A-Z0-9]{4}', icao):
        return icao
    return ''


def plausible_position(airports, plane):
    """A bounded great-circle corridor; rejects obviously unrelated routes."""
    try:
        coordinates = [(a['lat'], a['lon']) for a in airports] + [(plane['lat'], plane['lon'])]
        if any(not isinstance(v, (int, float)) or isinstance(v, bool) or not math.isfinite(v)
               for point in coordinates for v in point):
            return False
        if any(not -90 <= lat <= 90 or not -180 <= lon <= 180 for lat, lon in coordinates):
            return False
        a, b, p = [tuple(map(math.radians, point)) for point in coordinates]
        def angle(first, second):
            value = (math.sin((second[0] - first[0]) / 2) ** 2 + math.cos(first[0]) * math.cos(second[0])
                     * math.sin((second[1] - first[1]) / 2) ** 2)
            return 2 * math.asin(math.sqrt(min(1, max(0, value))))
        def bearing(first, second):
            delta = second[1] - first[1]
            return math.atan2(math.sin(delta) * math.cos(second[0]),
                              math.cos(first[0]) * math.sin(second[0])
                              - math.sin(first[0]) * math.cos(second[0]) * math.cos(delta))
        length = angle(a, b) * 6371.0088
        if length < 1:
            return False
        tolerance = max(92.6, length * 0.20)
        ap = angle(a, p)
        delta_bearing = bearing(a, p) - bearing(a, b)
        cross = abs(math.asin(max(-1, min(1, math.sin(ap) * math.sin(delta_bearing))))) * 6371.0088
        along = math.atan2(math.sin(ap) * math.cos(delta_bearing), math.cos(ap)) * 6371.0088
        return cross <= tolerance and -tolerance <= along <= length + tolerance
    except (KeyError, TypeError, ValueError):
        return False


def route_codes(raw, callsign, plane=None):
    # Standing data is an estimate, even when the service calls it plausible.
    # Reject ambiguous multi-stop itineraries and mismatched callsigns.
    if not isinstance(raw, dict) or raw.get('callsign') != callsign:
        return '', ''
    if 'plausible' in raw and raw['plausible'] is not True:
        return '', ''
    airports = raw.get('_airports')
    code_string = raw.get('airport_codes')
    if not isinstance(code_string, str):
        return '', ''
    codes = code_string.split('-')
    if not isinstance(airports, list) or len(airports) != 2 or len(codes) != 2:
        return '', ''
    if any(not isinstance(a, dict) for a in airports):
        return '', ''
    if [a.get('icao') for a in airports] != codes:
        return '', ''
    if plane is not None:
        if not plausible_position(airports, plane):
            return '', ''
    elif raw.get('plausible') is not True:
        return '', ''
    origin, destination = [airport_code(a) for a in airports]
    return (origin, destination) if origin and destination else ('', '')


def route_metadata(raw, callsign, plane=None):
    origin, destination = route_codes(raw, callsign, plane)
    if not origin:
        return '', '', '', ''
    return origin, destination, *(airport_city(a) for a in raw['_airports'])


def fetch_route(plane):
    callsign = plane['callsign']
    # Public standing-data endpoint used by ADS-B.lol's own route service.
    url = f"https://vrs-standing-data.adsb.lol/routes/{callsign[:2]}/{callsign}.json"
    request = urllib.request.Request(url, headers={
        'User-Agent': 'AirRadar/3.0 (cached route lookup)',
        'Accept': 'application/json'})
    with urllib.request.urlopen(request, timeout=10) as response:
        body = response.read(131073)
        if len(body) > 131072:
            raise ValueError('Odgovor za rutu je prevelik')
        return json.loads(body)


class RouteCache:
    def __init__(self, path=None):
        self.path = Path(path) if path else Path(__file__).resolve().parents[1] / '.routes-cache.json'
        self.cache = {}
        self.pending = set()
        self.lock = threading.Lock()
        self.jobs = queue.Queue(maxsize=24)
        self.stop = threading.Event()
        try:
            if self.path.stat().st_size <= 131072:
                raw = json.loads(self.path.read_text(encoding='utf-8'))
                for callsign, entry in raw.items():
                    if (re.fullmatch(r'[A-Z]{2,3}[A-Z0-9]{1,9}', callsign)
                            and isinstance(entry, list) and len(entry) in (3, 5)
                            and isinstance(entry[0], (int, float))
                            and time.time() < entry[0] <= time.time() + 1800
                            and all(isinstance(c, str) and (not c or re.fullmatch(r'[A-Z0-9]{3,4}', c)) for c in entry[1:3])
                            and all(isinstance(c, str) and len(c) <= 47 and re.fullmatch(r'[A-Za-z0-9 .()-]*', c)
                                    for c in entry[3:])):
                        # Upgrade cached Serbian spellings without waiting for a route refresh.
                        if len(entry) == 5:
                            entry[3:5] = [AIRPORT_CITIES.get(code, city) for code, city in zip(entry[1:3], entry[3:5])]
                        self.cache[callsign] = entry
        except (OSError, ValueError, AttributeError, TypeError):
            pass
        self.thread = threading.Thread(target=self.worker, daemon=True)
        self.thread.start()

    def enrich(self, packet):
        now = time.time()
        with self.lock:
            for plane in packet['ac']:
                plane['origin'], plane['destination'] = '', ''
                plane['origin_city'], plane['destination_city'] = '', ''
                callsign = plane.get('callsign', '')
                if not re.fullmatch(r'[A-Z]{2,3}[A-Z0-9]{1,9}', callsign):
                    continue
                entry = self.cache.get(callsign)
                if entry and entry[0] > now:
                    plane['origin'], plane['destination'] = entry[1:3]
                    if len(entry) == 5:
                        plane['origin_city'], plane['destination_city'] = entry[3:5]
                    else:
                        plane['origin_city'], plane['destination_city'] = [AIRPORT_CITIES.get(c, '') for c in entry[1:3]]
                # Upgrade legacy code-only entries in the background without losing positions.
                if (not entry or entry[0] <= now or len(entry) == 3 and bool(entry[1])) and callsign not in self.pending:
                    try:
                        self.jobs.put_nowait((dict(plane), time.monotonic()))
                        self.pending.add(callsign)
                    except queue.Full:
                        pass
        return packet

    def worker(self):
        next_lookup = 0
        while not self.stop.is_set():
            try:
                plane, queued = self.jobs.get(timeout=0.2)
            except queue.Empty:
                continue
            if self.stop.wait(max(0, next_lookup - time.monotonic())):
                break
            callsign = plane['callsign']
            if time.monotonic() - queued > 120:
                with self.lock:
                    self.pending.discard(callsign)
                continue
            try:
                origin, destination, origin_city, destination_city = route_metadata(fetch_route(plane), callsign, plane)
                ttl = 900 if origin else 300
                next_lookup = time.monotonic() + 15
                if origin:
                    LOG.info('Procena rute %s: %s > %s', callsign, origin, destination)
            except (urllib.error.URLError, TimeoutError, OSError, ValueError, TypeError, AttributeError) as error:
                origin = destination = ''
                origin_city = destination_city = ''
                ttl = 60
                retry = 60
                if isinstance(error, urllib.error.HTTPError):
                    if error.code == 404:
                        ttl = 300
                        retry = 15
                    elif error.code in (401, 403):
                        retry = 300
                    elif error.code == 429:
                        try:
                            retry = max(60, min(600, int(error.headers.get('Retry-After', '60'))))
                        except ValueError:
                            pass
                next_lookup = time.monotonic() + retry
                LOG.warning('Route for %s unavailable; aircraft positions continue', callsign)
                LOG.debug('Route request details: %s', error)
            with self.lock:
                self.cache[callsign] = [time.time() + ttl, origin, destination, origin_city, destination_city]
                self.pending.discard(callsign)
                # Keep metadata bounded; route failures never stop the live feed.
                self.cache = dict(sorted(self.cache.items(), key=lambda item: item[1][0], reverse=True)[:256])
                snapshot = dict(self.cache)
            try:
                temporary = self.path.with_suffix('.tmp')
                temporary.write_text(json.dumps(snapshot), encoding='utf-8')
                temporary.replace(self.path)
            except OSError:
                LOG.debug('Route cache was not saved')

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
