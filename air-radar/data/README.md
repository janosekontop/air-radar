# Embedded setup page and certificate bundle

`setup.html` is shared by the board's web server and the local USB setup helper. It needs no external assets. Neither file contains Wi-Fi credentials.

`x509_crt_bundle` contains 121 public roots from the certifi Mozilla CA bundle available on 2026-10-07. It uses the ESP-IDF 4.4 compressed subject / public-key format. The firmware verifies server chains and hostnames, with no insecure fallback.

Regenerate using Python with `cryptography` installed:

```text
python tools/make_ca_bundle.py /path/to/certifi/cacert.pem data/x509_crt_bundle
```

Source format: [ESP-IDF 4.4.7 generator](https://github.com/espressif/esp-idf/blob/v4.4.7/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py). The public roots originate from [certifi](https://github.com/certifi/python-certifi) / Mozilla, under [MPL 2.0](https://github.com/certifi/python-certifi/blob/master/LICENSE). Updating roots requires rebuilding and uploading firmware. `cryptography` is only needed to regenerate the bundle.

`route_ca.pem` is the public **GTS Root R4** certificate from the same certifi bundle. Route requests use this full certificate because the older ESP32 compressed-bundle callback cannot anchor the CDN's cross-signed chain at an intermediate trusted root. Standard certificate and hostname validation remains enabled; no removed legacy root has been restored. The certificate is valid through 2036-06-22; a service certificate-authority change may require an updated root and firmware.

The included `LICENSE.certifi` preserves the certificate bundle's MPL 2.0 license.
