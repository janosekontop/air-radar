import http.client,json,threading,unittest
from wifi_setup import SetupServer,validate_config,strict_json,page_policy

class FakeBoard:
    span=50
    def __init__(self):self.sent=[];self.keys=[]
    def send(self,packet):self.sent.append(dict(packet))
    def wait_for(self,kind,timeout):
        if kind=='network':return {'event':kind,'ssid':'Example network','has_wifi':True,'configured':False,'password':'hidden-network-secret','ap_password':'hidden-hotspot-secret'}
        return {'event':kind,'ok':True}
    def key(self,key):self.keys.append(key)

class SetupTests(unittest.TestCase):
    def setUp(self):
        self.board=FakeBoard();self.server=SetupServer(('127.0.0.1',0),self.board,b'<html><script>test()</script></html>')
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
    def tearDown(self):self.server.shutdown();self.server.server_close();self.thread.join()
    def request(self,path,data=None,**headers):
        conn=http.client.HTTPConnection('127.0.0.1',self.server.server_port,timeout=3)
        h={'Content-Type':'application/json'};h.update(headers)
        conn.request('POST' if data is not None else 'GET',path,json.dumps(data) if not isinstance(data,bytes) and data is not None else data,h)
        r=conn.getresponse();result=(r.status,r.read(),dict(r.getheaders()));conn.close();return result
    def config(self,**changes):
        value={'mode':'wifi','latitude':51.5,'longitude':-.12,'ssid':'Example network','password':'example-passphrase','token':self.server.token}
        value.update(changes);return value
    def test_status_redacts_both_secrets(self):
        code,raw,_=self.request('/api/info');self.assertEqual(code,200)
        self.assertNotIn(b'hidden-network-secret',raw);self.assertNotIn(b'hidden-hotspot-secret',raw)
        self.assertEqual(json.loads(raw)['token'],self.server.token)
    def test_credentials_forwarded_once_and_not_echoed(self):
        code,raw,_=self.request('/api/config',self.config());self.assertEqual(code,200)
        self.assertNotIn(b'example-passphrase',raw);self.assertEqual(len(self.board.sent),1)
        self.assertEqual(self.board.sent[0]['password'],'example-passphrase')
    def test_usb_needs_no_wifi(self):
        code,_,_=self.request('/api/config',self.config(mode='usb',password=None,ssid=None))
        self.assertEqual(code,200);self.assertNotIn('password',self.board.sent[0])
    def test_saved_wifi_does_not_resend_password(self):
        self.assertEqual(self.request('/api/config',self.config(keep_wifi=True))[0],200)
        self.assertNotIn('password',self.board.sent[0])
    def test_wrong_or_missing_token(self):
        for token in ('','wrong',None):self.assertEqual(self.request('/api/config',self.config(token=token))[0],403)
        self.assertEqual(self.board.sent,[])
    def test_foreign_origin_and_rebound_host(self):
        for headers in [{'Origin':'https://example.org'},{'Origin':'null'},{'Host':'attacker.invalid:'+str(self.server.server_port)},{'Sec-Fetch-Site':'cross-site'}]:
            self.assertEqual(self.request('/api/config',self.config(),**headers)[0],403)
            self.assertEqual(self.request('/api/info',**headers)[0],403)
        self.assertEqual(self.board.sent,[])
    def test_utf8_ssid_byte_limits(self):
        self.assertEqual(self.request('/api/config',self.config(ssid='é'*17))[0],400)
        self.assertEqual(self.request('/api/config',self.config(ssid='é'*16))[0],200)
    def test_bad_password_and_null_are_rejected(self):
        for changes in [{'ssid':'a\0b'},{'password':'a\0b'},{'password':'x'*65},{'password':'short'},{'password':'g'*64}]:
            self.assertEqual(self.request('/api/config',self.config(**changes))[0],400)
        self.assertEqual(self.board.sent,[])
    def test_hex_key_and_open_network(self):
        for password in ('','0123456789abcdef'*4):self.assertEqual(self.request('/api/config',self.config(password=password))[0],200)
    def test_invalid_location_and_types(self):
        for changes in [{'latitude':90},{'longitude':181},{'latitude':True},{'latitude':'51.5'},{'longitude':float('nan')},{'longitude':float('inf')},{'mode':'arbitrary'},{'keep_wifi':'yes'}]:
            self.assertEqual(self.request('/api/config',self.config(**changes))[0],400)
        self.assertEqual(self.board.sent,[])
    def test_only_named_controls(self):
        for key in ('p','r','s','n\n','reset_config'):
            self.assertEqual(self.request('/api/key',{'token':self.server.token,'key':key})[0],400)
        self.assertEqual(self.board.keys,[])
        self.assertEqual(self.request('/api/key',{'token':self.server.token,'key':'n'})[0],200)
        self.assertEqual(self.board.keys,['n'])
    def test_mutations_need_json(self):
        self.assertEqual(self.request('/api/config',self.config(),**{'Content-Type':'text/plain'})[0],400)
    def test_duplicate_json_and_oversized_body(self):
        self.assertEqual(self.request('/api/config',b'{"token":"a","token":"b"}')[0],400)
        self.assertEqual(self.request('/api/config',b'x'*1025)[0],400)
    def test_security_headers_and_script_hash(self):
        code,_,h=self.request('/');self.assertEqual(code,200)
        self.assertEqual(h['Cache-Control'],'no-store');self.assertEqual(h['X-Frame-Options'],'DENY')
        self.assertIn("'sha256-",h['Content-Security-Policy']);self.assertIn("frame-ancestors 'none'",h['Content-Security-Policy'])
    def test_reset_and_hotspot_are_explicit_protected_actions(self):
        for path,event in [('/api/reset','reset_config'),('/api/hotspot','open_setup')]:
            self.assertEqual(self.request(path,{'token':'wrong'})[0],403)
            self.assertEqual(self.request(path,{'token':self.server.token})[0],200)
            self.assertEqual(self.board.sent[-1]['name'],event)
    def test_loopback_only(self):
        with self.assertRaises(ValueError):SetupServer(('0.0.0.0',0),self.board,b'')
    def test_script_hash_uses_html_normalized_line_endings(self):
        self.assertEqual(page_policy(b'<script>\r\nx()\r\n</script>'),page_policy(b'<script>\nx()\n</script>'))
    def test_stopping_companion_requires_session_token(self):
        self.assertEqual(self.request('/api/stop',{'token':'wrong'})[0],403)
        self.assertEqual(self.request('/api/stop',{'token':self.server.token})[0],200)

if __name__=='__main__':unittest.main()
