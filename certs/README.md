# HTTPS trust roots

`google-roots.pem` is the public root certificate set downloaded from
https://pki.goog/roots.pem on 2026-10-07. It verifies Google Calendar and the
Codex/Claude usage endpoints, which currently use GTS certificate chains.
PEM anchors allow mbedTLS to terminate at the trusted GTS root even when the
server sends a cross-signature to an older root absent from Mozilla's bundle.

OAuth token refresh uses `roots.bundle`, the compact Arduino-ESP32 CA bundle generated
from `mozilla-roots.pem`, downloaded from https://curl.se/ca/cacert.pem on 2026-10-07.
It verifies the OpenAI and Claude login hosts without loading all root
certificates into internal RAM. Regenerate after updating the PEM with:

```sh
python3 scripts/make_cert_bundle.py
```

The generator needs the Python `cryptography` package. Ordinary PlatformIO builds
use the checked-in binary and do not need that package or network downloads.
Private service account keys do not belong in this directory.
