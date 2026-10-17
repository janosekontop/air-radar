import math
import json
import unittest
from radar_bridge import normalize, radius_nm, FeedFailure, LAT, LON

NOW = 1791378327.5


def plane(**values):
    item = {"hex": "123abc", "flight": "ASL123  ", "lat": LAT, "lon": LON,
            "alt_baro": 10000, "gs": 100, "track": 90, "seen_pos": 1}
    item.update(values)
    return item


def feed(*aircraft, **values):
    raw = {"now": int(NOW * 1000), "ac": list(aircraft)}
    raw.update(values)
    return raw


class BridgeTests(unittest.TestCase):
    def test_square_corners_are_in_request_circle(self):
        for span in (25, 50, 100):
            self.assertGreaterEqual(radius_nm(span) * 1.852, span / 2 * math.sqrt(2))

    def test_square_corner_is_kept_and_outside_edge_is_removed(self):
        # 24km east AND north is inside a 50km square, beyond a 25km circle.
        near_lat = LAT + math.degrees(24 / 6371.0088)
        near_lon = LON + math.degrees(24 / (6371.0088 * math.cos(math.radians((LAT + near_lat) / 2))))
        raw = feed(plane(lat=near_lat, lon=near_lon), plane(hex="outside", lat=LAT + 0.3))
        result = normalize(raw, 50, 1, NOW)
        self.assertEqual(len(result["ac"]), 1)
        self.assertEqual(result["ac"][0]["hex"], "123abc")

    def test_stale_ground_and_missing_position_are_removed(self):
        raw = feed(plane(hex="stale", seen_pos=31), plane(hex="ground", alt_baro="ground"),
                   plane(hex="missing", lat=None), plane(hex="unknown", seen_pos=None), plane())
        self.assertEqual([p["hex"] for p in normalize(raw, 50, 1, NOW)["ac"]], ["123abc"])

    def test_cache_age_is_added_to_position_age(self):
        self.assertEqual(len(normalize(feed(plane(seen_pos=25)), 50, 1, NOW + 8)["ac"]), 0)

    def test_old_feed_and_bad_clock_raise(self):
        for received in (NOW + 31, NOW - 11):
            with self.assertRaises(FeedFailure):
                normalize(feed(plane()), 50, 1, received)

    def test_optional_values_and_units(self):
        result = normalize(feed(plane()), 50, 9, NOW)["ac"][0]
        self.assertEqual(result["flight"], "ASL123")
        self.assertEqual(result["speed"], 185.2)
        self.assertNotIn("alt", result)
        unknown = normalize(feed(plane(alt_baro=None, gs=None, track=None)), 50, 1, NOW)["ac"][0]
        self.assertIsNone(unknown["speed"])
        self.assertIsNone(unknown["track"])

    def test_model_and_real_callsign_are_distinct_from_registration(self):
        result = normalize(feed(plane(flight='', r='YU-ABC', t='A320')), 50, 1, NOW)['ac'][0]
        self.assertEqual(result['flight'], 'YU-ABC')
        self.assertEqual(result['callsign'], '')
        self.assertEqual(result['model'], 'A320')

    def test_complete_64_aircraft_packet_fits_uart_buffer(self):
        items = [plane(hex=f'{i:06x}', flight='ABCDEFGHI123', t='ABCDEFGH') for i in range(64)]
        result = normalize(feed(*items), 50, 1, NOW)
        for item in result['ac']:
            item.update(origin='ABCD', destination='EFGH', origin_city='A' * 47, destination_city='B' * 47)
        self.assertLess(len(json.dumps(result, separators=(',', ':')).encode()), 32768)

    def test_nearest_64_and_duplicate_filter(self):
        items = [plane(hex=f"{i:06x}", lat=LAT + i * 0.0001) for i in range(80)]
        result = normalize(feed(*reversed(items), items[0]), 50, 1, NOW)["ac"]
        self.assertEqual(len(result), 64)
        self.assertEqual(result[0]["hex"], "000000")
        self.assertEqual(result[-1]["hex"], "00003f")

    def test_center_at_date_line_wraps_longitude(self):
        raw=feed(plane(lat=10,lon=-179.99))
        result=normalize(raw,50,1,NOW,center=(10,179.99))
        self.assertEqual(len(result['ac']),1)
        self.assertEqual((result['lat'],result['lon']),(10,179.99))

    def test_invalid_schema_and_nan(self):
        with self.assertRaises(FeedFailure):
            normalize(feed(ac={}), 50, 1, NOW)
        self.assertEqual(normalize(feed(plane(lat=float("nan"))), 50, 1, NOW)["ac"], [])


if __name__ == "__main__":
    unittest.main()
