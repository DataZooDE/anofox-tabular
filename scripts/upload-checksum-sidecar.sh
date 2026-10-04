#!/bin/bash
#
# Publish a SHA256 sidecar next to an extension archive that
# duckdb/scripts/extension-upload-single.sh has just uploaded.
#
# The sidecar is `sha256sum <name>.duckdb_extension.gz` of the compressed
# archive exactly as served, so clients (the anofox Python loader) can verify
# the download before decompressing it. Arguments mirror the upload script so
# both can be called with the same values:
#
# Usage: ./upload-checksum-sidecar.sh <name> <extension_version> <duckdb_version> <architecture> <s3_bucket> <copy_to_latest> <copy_to_versioned> [<path_to_ext>]
#
# Honours DUCKDB_DEPLOY_SCRIPT_MODE like the upload script: anything other than
# "for_real" performs a dry run.

set -euo pipefail

name="$1"
ext_version="$2"
duckdb_version="$3"
arch="$4"
bucket="$5"
copy_to_latest="$6"
copy_to_versioned="$7"
base_ext_dir="${8:-/tmp/extension}"

if [[ "$arch" == wasm* ]]; then
  echo "No checksum sidecar for WebAssembly builds, skipping.."
  exit 0
fi

if [ -z "${AWS_ACCESS_KEY_ID:-}" ]; then
  echo "No AWS key found, skipping.."
  exit 0
fi

dry_run_param=""
if [ "${DUCKDB_DEPLOY_SCRIPT_MODE:-}" != "for_real" ]; then
  dry_run_param="--dryrun"
fi

archive="$base_ext_dir/$name.duckdb_extension.compressed"
gz_name="$name.duckdb_extension.gz"
sidecar="$base_ext_dir/$gz_name.sha256"

if [ ! -f "$archive" ]; then
  echo "ERROR: compressed archive not found at $archive" >&2
  exit 1
fi

# sha256sum output format: "<digest>  <file name>"
digest="$(sha256sum "$archive" | cut -d' ' -f1)"
printf '%s  %s\n' "$digest" "$gz_name" > "$sidecar"
echo "SHA256($gz_name) = $digest"

if [[ "$copy_to_versioned" == 'true' ]]; then
  aws s3 cp "$sidecar" "s3://$bucket/$name/$ext_version/$duckdb_version/$arch/$gz_name.sha256" $dry_run_param --acl public-read --content-type="text/plain"
fi

if [[ "$copy_to_latest" == 'true' ]]; then
  aws s3 cp "$sidecar" "s3://$bucket/$duckdb_version/$arch/$gz_name.sha256" $dry_run_param --acl public-read --content-type="text/plain"
fi
