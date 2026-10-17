"""Local Air Radar setup and optional USB feed. No credential files or web hosting."""
import argparse
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import re
import secrets
import threading
import time
import webbrowser
from serial.tools import list_ports
from radar_bridge import Board, fetch, normalize, FeedFailure
from routes import RouteCache


def choose_port(requested=None):
    if requested:
        return requested
    ports = [p.device for p in list_ports.comports() if p.vid in (0x1a86, 0x10c4, 0x303a)]
    if len(ports) != 1:
        raise RuntimeError('Connect one radar by USB, or specify --port with its serial port.')
    return ports[0]


def page_policy(page):
    # HTML normalizes line endings before the browser checks inline-script hashes.
    page=page.replace(b'\r\n',b'\n').replace(b'\r',b'\n')
    hashes=[]
    for script in re.findall(rb'<script>(.*?)</script>', page, re.S):
        hashes.append("'sha256-"+base64.b64encode(hashlib.sha256(script).digest()).decode()+"'")
    return ("default-src 'none'; script-src "+' '.join(hashes)+
            "; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
            "base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'")


def validate_config(data):
    allowed={'mode','latitude','longitude','keep_wifi','ssid','password','token'}
    if not isinstance(data,dict) or set(data)-allowed:
        raise ValueError('Invalid settings')
    if data.get('mode') not in ('wifi','usb'):
        raise ValueError('Choose a data source')
    for key,limit in [('latitude',89.9),('longitude',180)]:
        value=data.get(key)
        if isinstance(value,bool) or not isinstance(value,(float,int)) or not math.isfinite(value) or abs(value)>limit:
            raise ValueError('Enter valid latitude and longitude')
    if not isinstance(data.get('keep_wifi',False),bool):
        raise ValueError('Invalid saved-network option')
    result={key:data[key] for key in ('mode','latitude','longitude')}
    result['keep_wifi']=data.get('keep_wifi',False)
    if result['mode']=='wifi' and not result['keep_wifi']:
        ssid,password=data.get('ssid'),data.get('password')
        if (not isinstance(ssid,str) or not isinstance(password,str) or '\0' in ssid or '\0' in password
                or not 1<=len(ssid.encode('utf-8'))<=32):
            raise ValueError('Check the network name')
        n=len(password.encode('utf-8'))
        if n!=0 and not 8<=n<=63 and not (n==64 and re.fullmatch(r'[0-9a-fA-F]{64}',password)):
            raise ValueError('Password must be empty, 8–63 bytes, or a 64-digit hexadecimal key')
        result.update(ssid=ssid,password=password)
    return result


def strict_json(raw):
    def pairs(items):
        result={}
        for key,value in items:
            if key in result:raise ValueError('Duplicate field')
            result[key]=value
        return result
    def constant(value):raise ValueError('Invalid number')
    return json.loads(raw,object_pairs_hook=pairs,parse_constant=constant)


class USBFeed:
    def __init__(self,server):
        self.server=server;self.stop=threading.Event();self.last_success=0.;self.error='';self.signature=None
        self.routes=RouteCache()
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()

    def run(self):
        sequence=0;next_fetch=0.;signature=None
        while not self.stop.wait(.5):
            try:
                info=self.server.command({'type':'command','name':'wifi_info'},'network')
                current=(info.get('latitude'),info.get('longitude'))
                if not info.get('configured') or info.get('mode')!='usb':
                    next_fetch=0.;self.last_success=0.;self.error='';continue
                if current!=signature:signature=current;next_fetch=0.;self.last_success=0.
                if time.monotonic()<next_fetch:continue
                sequence+=1
                span=getattr(self.server.board,'span',50)
                packet=self.routes.enrich(normalize(fetch(span,current),span,sequence,center=current))
                # Settings may have changed while an internet request was in flight.
                latest=self.server.command({'type':'command','name':'wifi_info'},'network')
                if latest.get('mode')!='usb' or (latest.get('latitude'),latest.get('longitude'))!=current:continue
                self.server.command(packet,'feed')
                self.last_success=time.monotonic();self.signature=current;self.error='';next_fetch=time.monotonic()+10
            except FeedFailure as error:
                self.error='Aircraft data is unavailable. Retrying shortly.';next_fetch=time.monotonic()+max(10,error.retry)
            except (OSError,RuntimeError,ValueError):
                self.error='USB data is unavailable. Check the cable and internet connection.'
                next_fetch=time.monotonic()+10

    def close(self):
        self.stop.set();self.thread.join(timeout=2);self.routes.close()


class SetupServer(ThreadingHTTPServer):
    daemon_threads=True
    def __init__(self,address,board,page,enable_feed=False):
        if address[0]!='127.0.0.1':raise ValueError('Setup must bind to loopback')
        self.board=board;self.page=page;self.csp=page_policy(page)
        self.token=secrets.token_hex(16);self.board_lock=threading.Lock();self.feed=None
        super().__init__(address,Handler)
        if enable_feed:self.feed=USBFeed(self)

    def command(self,packet,event):
        with self.board_lock:
            self.board.send(packet)
            return self.board.wait_for(event,timeout=8)

    def status(self):
        info=self.command({'type':'command','name':'wifi_info'},'network')
        info={k:v for k,v in info.items() if k not in ('password','ap_password')}
        info.update(token=self.token,transport='usb',pc_streaming=False,pc_error='')
        if self.feed:
            info['pc_streaming']=bool(info.get('mode')=='usb' and self.feed.signature==(info.get('latitude'),info.get('longitude'))
                                      and time.monotonic()-self.feed.last_success<25)
            info['pc_error']=self.feed.error
        return info


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup();self.connection.settimeout(5)

    def log_message(self,*args):pass

    def local_request(self):
        expected='127.0.0.1:'+str(self.server.server_port)
        origin=self.headers.get('Origin')
        return (self.headers.get('Host')==expected and
                (not origin or origin=='http://'+expected) and
                self.headers.get('Sec-Fetch-Site')!='cross-site')

    def reply(self,code,value,content_type='application/json'):
        body=value if isinstance(value,bytes) else json.dumps(value,allow_nan=False).encode()
        self.send_response(code)
        for name,val in [('Content-Type',content_type),('Cache-Control','no-store'),('X-Content-Type-Options','nosniff'),
                         ('Content-Security-Policy',self.server.csp),('X-Frame-Options','DENY'),('Referrer-Policy','no-referrer'),
                         ('Content-Length',str(len(body)))]:self.send_header(name,val)
        self.end_headers()
        try:self.wfile.write(body)
        except (BrokenPipeError,ConnectionResetError):pass

    def do_OPTIONS(self):self.reply(403,{'error':'Cross-origin setup is not allowed'})

    def do_GET(self):
        if not self.local_request():return self.reply(403,{'error':'Open setup on this computer'})
        try:
            if self.path=='/':return self.reply(200,self.server.page,'text/html; charset=utf-8')
            if self.path=='/api/info':return self.reply(200,self.server.status())
            return self.reply(404,{'error':'Not found'})
        except (OSError,RuntimeError,ValueError):return self.reply(503,{'error':'Board unavailable. Check USB and close other serial apps.'})

    def do_POST(self):
        if not self.local_request():return self.reply(403,{'error':'Open setup on this computer'})
        try:
            length=int(self.headers.get('Content-Length','0'))
            if not 0<length<=1024 or self.headers.get_content_type()!='application/json' or self.headers.get('Transfer-Encoding'):
                return self.reply(400,{'error':'Invalid request'})
            data=strict_json(self.rfile.read(length))
            if not isinstance(data,dict) or not isinstance(data.get('token'),str) or not secrets.compare_digest(data['token'],self.server.token):
                return self.reply(403,{'error':'Reload the setup page and try again'})
            if self.path=='/api/config':
                config=validate_config(data)
                result=self.server.command(dict(type='command',name='configure',**config),'configured')
                config.clear();data.clear()
                if self.server.feed:self.server.feed.last_success=0.
                return self.reply(200 if result.get('ok') else 400,{'ok':bool(result.get('ok')),'error':'' if result.get('ok') else 'Check the network and location settings'})
            if self.path=='/api/key':
                key=data.get('key')
                if not isinstance(key,str) or len(key)!=1 or key not in '+-na[]':return self.reply(400,{'error':'Unknown control'})
                with self.server.board_lock:self.server.board.key(key)
                return self.reply(200,{'ok':True})
            if self.path in ('/api/reset','/api/hotspot'):
                reset=self.path=='/api/reset'
                result=self.server.command({'type':'command','name':'reset_config' if reset else 'open_setup'},'reset' if reset else 'setup_opened')
                return self.reply(200,{'ok':bool(result.get('ok'))})
            if self.path=='/api/stop':
                self.reply(200,{'ok':True})
                threading.Thread(target=self.server.shutdown,daemon=True).start()
                return
            return self.reply(404,{'error':'Not found'})
        except (ValueError,TypeError,UnicodeError):return self.reply(400,{'error':'Check the submitted settings'})
        except (OSError,RuntimeError):return self.reply(503,{'error':'Board unavailable. Check USB and try again.'})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port');parser.add_argument('--http-port',type=int,default=8765)
    parser.add_argument('--open-browser',action='store_true');args=parser.parse_args()
    board=Board(choose_port(args.port),50);server=None
    try:
        board.send({'type':'command','name':'hello'});board.wait_for('ready')
        server=SetupServer(('127.0.0.1',args.http_port),board,(Path(__file__).resolve().parents[1]/'data/setup.html').read_bytes(),True)
        url=f'http://127.0.0.1:{server.server_port}/';print('Radar setup: '+url,flush=True)
        if args.open_browser:webbrowser.open(url)
        server.serve_forever()
    except KeyboardInterrupt:return 0
    finally:
        if server:
            if server.feed:server.feed.close()
            server.server_close()
        board.close()

if __name__=='__main__':raise SystemExit(main())
