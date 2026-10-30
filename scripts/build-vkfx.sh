#!/bin/sh
# Build MangoWM against the patched wlroots (wlroots-vkfx) and SceneFX trees.
#
# This does NOT need either library installed. Both projects generate meson
# "uninstalled" pkg-config files in their build directories, so pointing
# PKG_CONFIG_PATH at those is enough to compile and link against the trees
# directly. Editing and rebuilding wlroots or SceneFX is immediately visible to
# the next MangoWM build, with no install step in between.
#
# Usage:
#   scripts/build-vkfx.sh            # configure (if needed) and build
#   scripts/build-vkfx.sh --reconf   # force a meson reconfigure first
#   scripts/build-vkfx.sh --run      # build, then exec the compositor
#   scripts/build-vkfx.sh --run -- -c ~/.config/mango/config.conf
#
# Location of the sibling trees, if they are not where this checkout is:
#   WLR_DIR=/path/to/wlroots   SFX_DIR=/path/to/scenefx   scripts/build-vkfx.sh
#
# By default this walks up from the mango checkout looking for sibling wlroots
# and scenefx trees.

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

BUILD_DIR=${BUILD_DIR:-$repo/build-vkfx}

# Walk up from this checkout looking for a sibling tree. Bare-worktree setups
# nest the checkout a few levels deep, so the search cannot assume a fixed
# depth. Each candidate is a glob so a single entry covers both the plain clone
# and the worktree layout.
find_dep() {
	_candidates=$1
	_dir=$repo
	while [ "$_dir" != "/" ]; do
		for _pat in $_candidates; do
			# shellcheck disable=SC2086
			for _hit in $_dir/$_pat; do
				if [ -f "$_hit/build/build.ninja" ] || [ -f "$_hit/meson.build" ]; then
					printf '%s\n' "$_hit"
					return 0
				fi
			done
		done
		_dir=$(dirname -- "$_dir")
	done
	return 1
}

WLR_DIR=${WLR_DIR:-$(find_dep "wlroots" || true)}
SFX_DIR=${SFX_DIR:-$(find_dep "scenefx" || true)}

reconf=0
run=0
# Arguments after "--" are passed through to the compositor when running.
passthru=
for arg in "$@"; do
	if [ "$arg" = "--" ]; then passthru=1; continue; fi
	if [ -n "$passthru" ]; then continue; fi
	case $arg in
	--reconf) reconf=1 ;;
	--run) run=1 ;;
	-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown argument: $arg (use -- to pass it to mango)" >&2; exit 2 ;;
	esac
done

die() { echo "error: $*" >&2; exit 1; }

# --- locate the two dependency trees ----------------------------------------

[ -d "$WLR_DIR" ] || die "wlroots tree not found: $WLR_DIR
Set WLR_DIR=/path/to/wlroots to point at it."
[ -d "$SFX_DIR" ] || die "scenefx tree not found: $SFX_DIR
Set SFX_DIR=/path/to/scenefx to point at it."

# --- make sure both dependencies are configured and built -------------------
#
# Their uninstalled .pc files are generated at configure time, but the headers
# they point at (e.g. wlr/version.h) and the libraries only appear once built.

build_dep() {
	dir=$1
	name=$2
	if [ ! -f "$dir/build/build.ninja" ]; then
		echo "==> configuring $name"
		meson setup "$dir/build" "$dir" >/dev/null \
			|| die "meson setup failed for $name"
	fi
	echo "==> building $name"
	ninja -C "$dir/build" >/dev/null || die "build failed for $name"
}

build_dep "$WLR_DIR" wlroots
build_dep "$SFX_DIR" scenefx

# --- point pkg-config at the uninstalled trees ------------------------------
#
# Order matters: both trees provide a wlroots pkg-config file, and the fork
# installs it as wlroots-vkfx-0.20. meson.build prefers that name, so having
# the fork's directory first is what makes it win over a system wlroots.

PKG_CONFIG_PATH="$WLR_DIR/build/meson-uninstalled:$SFX_DIR/build/meson-uninstalled${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

# --- configure and build MangoWM -------------------------------------------

if [ "$reconf" = 1 ] && [ -f "$BUILD_DIR/build.ninja" ]; then
	echo "==> reconfiguring mango"
	meson configure "$BUILD_DIR" --wipe >/dev/null 2>&1 || true
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
	echo "==> configuring mango"
	meson setup "$BUILD_DIR" "$repo" \
		--buildtype=debugoptimized \
		> "$BUILD_DIR.log" 2>&1 || {
			echo "meson setup failed, see $BUILD_DIR.log" >&2
			tail -20 "$BUILD_DIR.log" >&2
			exit 1
		}
else
	echo "==> configuring mango (incremental)"
	meson setup --reconfigure "$BUILD_DIR" "$repo" \
		--buildtype=debugoptimized \
		> "$BUILD_DIR.log" 2>&1 || {
			echo "meson reconfigure failed, see $BUILD_DIR.log" >&2
			tail -20 "$BUILD_DIR.log" >&2
			exit 1
		}
fi

# Confirm the intended trees were actually picked, rather than a system copy.
wlr_prefix=$(pkg-config --variable=libdir wlroots-vkfx-0.20 2>/dev/null || true)
case $wlr_prefix in
"$WLR_DIR"/build*) ;;
*) echo "warning: wlroots-vkfx-0.20 did not resolve to $WLR_DIR/build" >&2
   echo "         got: ${wlr_prefix:-<not found>}" >&2 ;;
esac

echo "==> building mango"
ninja -C "$BUILD_DIR" || exit 1

printf '\n==> done: %s\n' "$BUILD_DIR/mango"

# --- running ----------------------------------------------------------------
#
# The uninstalled .pc files point at the libraries in each build directory, and
# meson turns those absolute paths into a build rpath on the executable, so the
# runtime linker already finds the fork's libraries without help.
# LD_LIBRARY_PATH is set anyway because it costs nothing and wins over the
# rpath, which is what you want if a copy of either library also happens to be
# installed system-wide.

RUNTIME_LIBS="$WLR_DIR/build:$SFX_DIR/build"

if [ "$run" = 1 ]; then
	echo "==> starting mango (WLR_RENDERER=${WLR_RENDERER:-vulkan})"
	# Everything after "--" is handed to the compositor.
	mango_args=
	if [ -n "${passthru:-}" ]; then
		seen=
		for a in "$@"; do
			if [ -n "$seen" ]; then mango_args="$mango_args $a"; fi
			if [ "$a" = "--" ]; then seen=1; fi
		done
	fi
	# shellcheck disable=SC2086
	LD_LIBRARY_PATH="$RUNTIME_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
	WLR_RENDERER=${WLR_RENDERER:-vulkan} \
		exec "$BUILD_DIR/mango" $mango_args
fi

cat <<EOF

To run it:

  LD_LIBRARY_PATH=$RUNTIME_LIBS \\
  WLR_RENDERER=vulkan $BUILD_DIR/mango

(or re-run this script with --run)
EOF
