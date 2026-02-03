#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 <path/to/ai-z_*.deb> <out_repo_dir> [suite] [component]" >&2
  echo "Example: $0 ./ai-z_0.1.0_amd64.deb ./repo stable main" >&2
  exit 2
fi

deb_path="$1"
out_dir="$2"
suite="${3:-stable}"
component="${4:-main}"

if [[ ! -f "$deb_path" ]]; then
  echo "ERROR: deb not found: $deb_path" >&2
  exit 1
fi

if ! command -v dpkg-scanpackages >/dev/null 2>&1; then
  echo "ERROR: dpkg-scanpackages not found. Install: sudo apt-get install -y dpkg-dev" >&2
  exit 1
fi

arch="$(dpkg-deb -f "$deb_path" Architecture)"
pkg="$(dpkg-deb -f "$deb_path" Package)"

pool_dir="$out_dir/pool/$component/${pkg:0:1}/$pkg"
dists_dir="$out_dir/dists/$suite/$component/binary-$arch"

mkdir -p "$pool_dir" "$dists_dir"

cp -f "$deb_path" "$pool_dir/"

# Generate Packages and Packages.gz
(
  cd "$out_dir"
  dists_dir_rel="dists/$suite/$component/binary-$arch"
  dpkg-scanpackages "pool" /dev/null > "$dists_dir_rel/Packages"
  gzip -fk "$dists_dir_rel/Packages"
)

# Release file (not signed). For real distribution, sign Release/InRelease.
# We generate it ourselves to ensure Suite/Codename are present (avoids apt warnings).
(
  cd "$out_dir"

  release_dir="dists/$suite"
  release_file="$release_dir/Release"

  mkdir -p "$release_dir"

  {
    echo "Origin: $pkg"
    echo "Label: $pkg"
    echo "Suite: $suite"
    echo "Codename: $suite"
    echo "Date: $(date -Ru)"
    echo "Architectures: $arch"
    echo "Components: $component"
    echo "Description: $pkg APT repository"

    echo "MD5Sum:"
    (cd "$release_dir" && find . -type f ! -name 'Release' ! -name 'InRelease' ! -name 'Release.gpg' -print0 \
      | LC_ALL=C sort -z \
      | while IFS= read -r -d '' f; do
          rel="${f#./}"
          size="$(stat -c%s "$rel")"
          sum="$(md5sum "$rel" | awk '{print $1}')"
          printf " %s %16s %s\n" "$sum" "$size" "$rel"
        done)

    echo "SHA1:"
    (cd "$release_dir" && find . -type f ! -name 'Release' ! -name 'InRelease' ! -name 'Release.gpg' -print0 \
      | LC_ALL=C sort -z \
      | while IFS= read -r -d '' f; do
          rel="${f#./}"
          size="$(stat -c%s "$rel")"
          sum="$(sha1sum "$rel" | awk '{print $1}')"
          printf " %s %16s %s\n" "$sum" "$size" "$rel"
        done)

    echo "SHA256:"
    (cd "$release_dir" && find . -type f ! -name 'Release' ! -name 'InRelease' ! -name 'Release.gpg' -print0 \
      | LC_ALL=C sort -z \
      | while IFS= read -r -d '' f; do
          rel="${f#./}"
          size="$(stat -c%s "$rel")"
          sum="$(sha256sum "$rel" | awk '{print $1}')"
          printf " %s %16s %s\n" "$sum" "$size" "$rel"
        done)
  } > "$release_file"
)

echo "Repo generated at: $out_dir"
echo "Host it over HTTPS, then add on clients (unsigned example):"
echo "  deb [trusted=yes] https://YOUR_HOST/$(basename "$out_dir") $suite $component"
