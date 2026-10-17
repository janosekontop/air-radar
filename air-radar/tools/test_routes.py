import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
from routes import RouteCache, route_codes, route_metadata, airport_city


def route(**values):
    result = {'callsign': 'ASL123', 'plausible': True, 'airport_codes': 'LYBE-LTFM',
              '_airports': [{'icao': 'LYBE', 'iata': 'BEG'}, {'icao': 'LTFM', 'iata': 'IST'}]}
    result.update(values)
    return result


class RouteTests(unittest.TestCase):
    def test_iata_codes_and_icao_fallback(self):
        self.assertEqual(route_codes(route(), 'ASL123'), ('BEG', 'IST'))
        raw = route(_airports=[{'icao': 'LYBE', 'iata': ''}, {'icao': 'LTFM', 'iata': 'IST'}])
        self.assertEqual(route_codes(raw, 'ASL123'), ('LYBE', 'IST'))

    def test_unverified_wrong_callsign_and_multistop_are_unknown(self):
        for raw in (route(plausible=False), route(plausible=None), route(callsign='OTHER'),
                    route(airport_codes='LYBE-LTFM-EGSS'), route(_airports=[]), {}, None):
            self.assertEqual(route_codes(raw, 'ASL123'), ('', ''))

    def test_swapped_airports_and_bad_code_are_rejected(self):
        raw = route(_airports=[{'icao': 'LTFM', 'iata': 'IST'}, {'icao': 'LYBE', 'iata': 'BEG'}])
        self.assertEqual(route_codes(raw, 'ASL123'), ('', ''))
        raw = route(_airports=[{'icao': 'LYBE', 'iata': '!!!'}, {'icao': 'LTFM', 'iata': 'IST'}])
        self.assertEqual(route_codes(raw, 'ASL123'), ('LYBE', 'IST'))

    def test_static_route_needs_a_plausible_position(self):
        raw = route()
        del raw['plausible']
        raw['_airports'][0].update(lat=0, lon=0)
        raw['_airports'][1].update(lat=0, lon=20)
        self.assertEqual(route_codes(raw, 'ASL123'), ('', ''))
        self.assertEqual(route_codes(raw, 'ASL123', {'lat': 0, 'lon': 10}), ('BEG', 'IST'))
        self.assertEqual(route_codes(raw, 'ASL123', {'lat': 45, 'lon': 10}), ('', ''))
        self.assertEqual(route_codes(raw, 'ASL123', {'lat': 0, 'lon': 100}), ('', ''))
        self.assertEqual(route_codes(raw, 'ASL123', {'lat': float('nan'), 'lon': 10}), ('', ''))

    def test_cache_enriches_without_a_request_and_expired_route_does_not_leak(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'cache.json'
            path.write_text(json.dumps({'ASL123': [time.time() + 300, 'BEG', 'IST', 'Belgrade', 'Istanbul']}))
            with patch.object(RouteCache, 'worker', return_value=None):
                cache = RouteCache(path)
                try:
                    packet = {'ac': [{'callsign': 'ASL123', 'lat': 51.5, 'lon': -0.12}]}
                    self.assertEqual(cache.enrich(packet)['ac'][0]['origin'], 'BEG')
                    self.assertEqual(packet['ac'][0]['origin_city'], 'Belgrade')
                    self.assertEqual(cache.jobs.qsize(), 0)
                    cache.cache['ASL123'][0] = time.time() - 1
                    self.assertEqual(cache.enrich(packet)['ac'][0]['origin'], '')
                    self.assertEqual(packet['ac'][0]['origin_city'], '')
                    cache.enrich(packet)
                    self.assertEqual(cache.jobs.qsize(), 1)
                finally:
                    cache.close()

    def test_full_city_names_and_english_names(self):
        self.assertEqual(route_metadata(route(), 'ASL123'), ('BEG', 'IST', 'Belgrade', 'Istanbul'))
        self.assertEqual(airport_city({'iata': 'OTP', 'location': 'Bucharest'}), 'Bucharest')
        self.assertEqual(airport_city({'iata': 'XYZ', 'location': 'Santa Cruz de Tenerife'}), 'Santa Cruz de Tenerife')
        self.assertEqual(airport_city({'iata': 'XYZ', 'location': 'İzmir'}), 'Izmir')
        self.assertEqual(airport_city({'iata': 'XYZ', 'location': 'A"\nB'}), 'A B')
        self.assertEqual(airport_city({'iata': 'XYZ', 'location': 'A' * 200}), 'A' * 47)
        self.assertEqual(route_metadata(route(plausible=False), 'ASL123'), ('', '', '', ''))

    def test_legacy_cache_is_upgraded_without_losing_route(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'cache.json'
            path.write_text(json.dumps({'ASL123': [time.time() + 300, 'BEG', 'IST']}))
            with patch.object(RouteCache, 'worker', return_value=None):
                cache = RouteCache(path)
                try:
                    packet = {'ac': [{'callsign': 'ASL123', 'lat': 51.5, 'lon': -0.12}]}
                    item = cache.enrich(packet)['ac'][0]
                    self.assertEqual((item['origin'], item['origin_city']), ('BEG', 'Belgrade'))
                    cache.enrich(packet)
                    self.assertEqual(cache.jobs.qsize(), 1)
                finally:
                    cache.close()

    def test_cached_serbian_city_names_are_upgraded_to_english(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'cache.json'
            path.write_text(json.dumps({'ASL123': [time.time() + 300, 'BEG', 'OTP', 'Beograd', 'Bukurest']}))
            with patch.object(RouteCache, 'worker', return_value=None):
                cache = RouteCache(path)
                try:
                    item = cache.enrich({'ac': [{'callsign': 'ASL123'}]})['ac'][0]
                    self.assertEqual((item['origin_city'], item['destination_city']), ('Belgrade', 'Bucharest'))
                finally:
                    cache.close()

    def test_lookup_failure_keeps_positions_and_reports_unknown_route(self):
        with tempfile.TemporaryDirectory() as folder:
            with patch('routes.fetch_route', side_effect=OSError('offline')):
                cache = RouteCache(Path(folder) / 'cache.json')
                try:
                    packet = {'ac': [{'callsign': 'ASL123', 'lat': 51.5, 'lon': -0.12}]}
                    cache.enrich(packet)
                    deadline = time.monotonic() + 2
                    while 'ASL123' not in cache.cache and time.monotonic() < deadline:
                        time.sleep(0.01)
                    item = cache.enrich(packet)['ac'][0]
                    self.assertEqual(item['lat'], 51.5)
                    self.assertEqual(item['origin'], '')
                    self.assertEqual(item['destination'], '')
                    self.assertIn('ASL123', cache.cache)
                finally:
                    cache.close()


if __name__ == '__main__':
    unittest.main()
