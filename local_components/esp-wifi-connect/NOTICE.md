# esp-wifi-connect (local copy)

This directory is a modified copy of the `78/esp-wifi-connect` component (version 3.3.1, MIT
licence, https://github.com/78/esp-wifi-connect). The upstream `LICENSE` file is not part of this
copy: keep the upstream licence notice when you redistribute it.

Changes (marked `[SANDBOX]` in the code): WPA2/WPA3-Enterprise (`wifi_enterprise.*`), captive-portal
login (`wifi_portal.*`), server profiles (`server_profiles.*`), an account field and a server tab on
the setup page, a username in `SsidManager`.

Not byte-identical to upstream: `dns_server.*` and `assets/wifi_configuration*.html` were re-written
from the interface the rest of the component uses, because the original files were not available
when this copy was restored.
