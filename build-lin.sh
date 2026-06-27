#!/usr/bin/env -S bash -Eeuo pipefail
cd "$(dirname -- "$(readlink -f -- "$0")")"

build_dir="${BUILD_DIR:-build-linux}"

run_with_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout 10m "$@"
  else
    "$@"
  fi
}

echo "==> Configure ${build_dir}"
run_with_timeout cmake -S . -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_QUIET=OFF

echo "==> Build"
cmake --build "${build_dir}" --config Release --parallel

echo "==> Strip"
if command -v sstrip >/dev/null 2>&1; then
  sstrip "${build_dir}/q2manager"
elif command -v strip >/dev/null 2>&1; then
  strip "${build_dir}/q2manager"
else
  echo "strip/sstrip not found; skipping"
fi

echo "==> Dist"
mkdir -p dist
cp -f "${build_dir}/q2manager" dist/q2manager
cp -a assets dist/
echo "dist/q2manager ready"
