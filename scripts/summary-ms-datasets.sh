#!/usr/bin/env bash
#
# Run `rastro summary` over every MeasurementSet (*.ms directory) found under
# the given roots. Intended as an optional integration test against real
# datasets, e.g. RASTRO_MS_DATASETS=/path/to/datasets ctest.
#
# Usage: summary-ms-datasets.sh <rastro-binary> <root> [root...]
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "usage: $0 <rastro-binary> <root> [root...]" >&2
  exit 2
fi

rastro="$1"
shift

status=0
while IFS= read -r ms; do
  echo "==> summary ${ms}"
  if ! "${rastro}" summary "${ms}" > /dev/null; then
    echo "FAILED: ${ms}" >&2
    status=1
  fi
done < <(find "$@" -type d -name '*.ms' 2>/dev/null)

exit "${status}"
