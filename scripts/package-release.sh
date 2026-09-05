#!/usr/bin/env bash
set -Eeuo pipefail

image="${1:?usage: package-release.sh IMAGE PG_MAJOR VERSION}"
pg_major="${2:?usage: package-release.sh IMAGE PG_MAJOR VERSION}"
version="${3:?usage: package-release.sh IMAGE PG_MAJOR VERSION}"
extension_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dist_dir="${extension_dir}/dist"
package="pg_unitree_r1-${version}-pg${pg_major}-linux-x86_64"
package_dir="${dist_dir}/${package}"
container=""

if [[ ! "${pg_major}" =~ ^[0-9]+$ ]]; then
  echo "invalid PostgreSQL major version: ${pg_major}" >&2
  exit 1
fi

if [[ ! "${version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
  echo "invalid extension version: ${version}" >&2
  exit 1
fi

cleanup() {
  if [[ -n "${container}" ]]; then
    docker rm --force "${container}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

rm -rf -- "${package_dir}"
mkdir -p \
  "${package_dir}/usr/lib/postgresql/${pg_major}/lib" \
  "${package_dir}/usr/share/postgresql/${pg_major}/extension"

docker_destination="${package_dir}"
case "$(uname -s)" in
  MINGW* | MSYS*)
    export MSYS_NO_PATHCONV=1
    docker_destination="$(cygpath -w "${package_dir}")"
    ;;
esac

container="$(docker create "${image}")"
for library in pg_unitree_r1.so libddsc.so.0 libddscxx.so.0; do
  docker cp \
    "${container}:/usr/lib/postgresql/${pg_major}/lib/${library}" \
    "${docker_destination}/usr/lib/postgresql/${pg_major}/lib/${library}"
done

for metadata in pg_unitree_r1.control "pg_unitree_r1--${version}.sql"; do
  docker cp \
    "${container}:/usr/share/postgresql/${pg_major}/extension/${metadata}" \
    "${docker_destination}/usr/share/postgresql/${pg_major}/extension/${metadata}"
done

archive="${dist_dir}/${package}.tar.gz"
rm -f -- "${archive}" "${archive}.sha256"
tar -C "${dist_dir}" -czf "${archive}" "${package}"
(
  cd -- "${dist_dir}"
  sha256sum "${package}.tar.gz" > "${package}.tar.gz.sha256"
)

echo "created ${archive}"
