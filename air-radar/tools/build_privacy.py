"""Keep local build-directory names out of distributed firmware strings."""
from pathlib import Path
import re, base64, hashlib, json
Import('env')
home = Path.home().as_posix()
env.Append(CCFLAGS=['-ffile-prefix-map='+home+'=/build',
                    '-fmacro-prefix-map='+home+'=/build'])
root=Path(env['PROJECT_DIR'])
page=(root/'data/setup.html').read_bytes().replace(b'\r\n',b'\n').replace(b'\r',b'\n')
hashes=["'sha256-"+base64.b64encode(hashlib.sha256(s).digest()).decode()+"'"
        for s in re.findall(rb'<script>(.*?)</script>',page,re.S)]
policy=("default-src 'none'; script-src "+' '.join(hashes)+
        "; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; "
        "base-uri 'none'; object-src 'none'; frame-ancestors 'none'; form-action 'self'")
(root/'include/web_security.h').write_text('#pragma once\nstatic const char pageCsp[] = '+json.dumps(policy)+';\n')
