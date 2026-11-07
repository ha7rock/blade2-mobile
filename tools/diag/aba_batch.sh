#!/bin/bash
# Run several game-frame configurations back to back: tools/diag/aba_batch.sh "<tag> <ENV=V ...>" ...  (each with ABA_DRAWSET=1 DM_PRESENT_LINK=0 unless overridden)
HERE=$(cd "$(dirname "$0")/../.." && pwd)
for spec in "$@"; do
  set -- $spec; tag=$1; shift
  env ABA_DRAWSET=1 DM_PRESENT_LINK=${DM_PRESENT_LINK:-0} "$@" "$HERE/tools/diag/aba_run.sh" "$tag" 55 0 ${ABA_WATCH:-100} > "${DM_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)/work}/run/aba/$tag.out" 2>&1
  echo "$tag done"
done
echo ALLDONE
