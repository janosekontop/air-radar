"""Verify packaged firmware hashes without connecting to hardware."""
import hashlib
import json
from pathlib import Path


def main():
    folder = Path(__file__).resolve().parents[1] / 'Firmware'
    manifest = json.loads((folder / 'manifest.json').read_text())
    expected = {'bootloader.bin': '0x0', 'partitions.bin': '0x8000',
                'boot_app0.bin': '0xe000', 'firmware.bin': '0x10000'}
    entries = manifest['files']
    if len(entries) != len(expected) or {item['name'] for item in entries} != set(expected):
        raise RuntimeError('Unexpected firmware file list')
    for item in entries:
        if item['offset'] != expected[item['name']]:
            raise RuntimeError('Unexpected flash offset')
        raw = (folder / item['name']).read_bytes()
        if len(raw) != item['bytes'] or hashlib.sha256(raw).hexdigest() != item['sha256']:
            raise RuntimeError('Firmware verification failed: ' + item['name'])
    print('All four firmware images match the release manifest.')


if __name__ == '__main__':
    main()
