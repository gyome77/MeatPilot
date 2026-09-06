#!/usr/bin/env bash
# core/ must stay free of platform dependencies. That is what lets the whole
# acceptance suite run on the host, so it is enforced rather than trusted.
set -euo pipefail

cd "$(dirname "$0")/.."

forbidden='esp_|freertos/|driver/|Arduino\.h|nvs_|lwip/|sdkconfig'
if grep -rInE "#include[[:space:]]*[<\"]($forbidden)" core/ ; then
  echo
  echo "ERROR: core/ must not depend on any platform header." >&2
  echo "Move the dependency behind a platform/ interface instead." >&2
  exit 1
fi

# The engine must not read a clock either: time is injected so that timing
# rules are testable by advancing a number.
if grep -rInE '\b(time|clock|gettimeofday|steady_clock|system_clock)\s*\(' core/ ; then
  echo
  echo "ERROR: core/ must not read a clock. Inject the timestamp." >&2
  exit 1
fi

echo "core/ purity: ok"
