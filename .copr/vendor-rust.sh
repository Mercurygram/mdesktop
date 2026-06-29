#!/bin/sh
# Builds the rust.tar.gz Copr source: tlottie and wallet-engine at the pins of
# the release's snap/snapcraft.yaml, joined into the tdesktop_rust crate the
# way the centos_env Docker stage does, with every crates.io dependency vendored
# (plus the C++ bindgen's) so the Copr build itself can run cargo offline.
#
# Usage: vendor-rust.sh <snapcraft.yaml> <output.tar.gz>
set -eu

snap=$(realpath "$1")
out=$(realpath -m "$2")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/rust"
cd "$work/rust"

pin() { # snapcraft part -> source-commit
	awk -v part="  $1:" '$0 == part { found = 1; next }
		found && /source-commit:/ { print $2; exit }' "$snap"
}

fetch() { # snapcraft part, url
	sha=$(pin "$1")
	test -n "$sha" || { echo "no source-commit for $1" >&2; exit 1; }
	git init -q "$1"
	git -C "$1" fetch -q --depth=1 "$2" "$sha"
	# wallet-engine keeps example assets in LFS; the build does not need them.
	git -C "$1" -c filter.lfs.process= -c filter.lfs.smudge=cat \
		-c filter.lfs.required=false checkout -q FETCH_HEAD
}

fetch patches https://github.com/desktop-app/patches.git
fetch tlottie https://github.com/dkaraush/tlottie.git
fetch wallet-engine https://github.com/i582/wallet-engine.git
git -C wallet-engine apply ../patches/wallet-engine.patch
rm -rf patches tlottie/.git wallet-engine/.git

cargo new -q --lib --vcs none tdesktop_rust
printf 'pub use ::tlottie;\npub use ::wallet_engine;\n' > tdesktop_rust/src/lib.rs
cargo add -q --manifest-path tdesktop_rust/Cargo.toml --path tlottie --features c-api
cargo add -q --manifest-path tdesktop_rust/Cargo.toml --path wallet-engine
cargo generate-lockfile -q --manifest-path tdesktop_rust/Cargo.toml

mkdir .cargo
cargo vendor --locked --manifest-path tdesktop_rust/Cargo.toml \
	--sync wallet-engine/bindgen/cpp/Cargo.toml vendor > .cargo/config.toml

cd "$work"
tar -czf "$out" rust
