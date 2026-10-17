"""Program the supported 16 MB Waveshare board with checked release files."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
from wifi_setup import choose_port

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port');parser.add_argument('--factory-reset',action='store_true',help='Erase ALL flash data for a new owner before programming')
    args=parser.parse_args();folder=Path(__file__).resolve().parents[1]/'Firmware'
    manifest=json.loads((folder/'manifest.json').read_text())
    for item in manifest['files']:
        path=folder/item['name']
        if hashlib.sha256(path.read_bytes()).hexdigest()!=item['sha256']:raise RuntimeError('Firmware integrity check failed')
    try:
        __import__('esptool');tool=[sys.executable,'-m','esptool']
    except ImportError:
        installed=Path.home()/'.platformio/packages/tool-esptoolpy/esptool.py'
        if not installed.is_file():raise RuntimeError('Install esptool 4.5.1 with: python -m pip install esptool==4.5.1')
        tool=[sys.executable,str(installed)]
    prefix=tool+['--chip','esp32s3','--port',choose_port(args.port),'--baud','460800']
    probe=subprocess.run(prefix+['flash_id'],capture_output=True,text=True,check=True)
    if 'Detected flash size: 16MB' not in probe.stdout:raise RuntimeError('This release requires the 16 MB Waveshare ESP32-S3-LCD-1.3 board')
    if args.factory_reset:subprocess.run(prefix+['erase_flash'],check=True)
    command=prefix+['write_flash','-z','--flash_mode','keep','--flash_freq','keep','--flash_size','16MB']
    for item in manifest['files']:command += [item['offset'],str(folder/item['name'])]
    subprocess.run(command,check=True)
    print('Air Radar installed. Open Start Setup.cmd, or use the setup hotspot shown on the LCD.')

if __name__=='__main__':main()
