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
#   scripts/build-vkfx.sh --verbose     # also print each dependency's full log
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
verbose=0
# Arguments after "--" are passed through to the compositor when running.
passthru=
for arg in "$@"; do
	if [ "$arg" = "--" ]; then passthru=1; continue; fi
	if [ -n "$passthru" ]; then continue; fi
	case $arg in
	--reconf) reconf=1 ;;
	--run) run=1 ;;
	--verbose|-v) verbose=1 ;;
	-h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

run_quiet() {
	_name=$1
	_log=$2
	shift 2
	# The build directory may not exist yet (first configure), but the log has to
	# live inside it, so create the parent first.
	mkdir -p "$(dirname -- "$_log")"
	if [ "${verbose:-0}" = 1 ]; then
		"$@" >"$_log" 2>&1
		_rc=$?
		cat "$_log"
		return $_rc
	fi
	if ! "$@" >"$_log" 2>&1; then
		# Lead with the errors themselves. In a long build the failing steps can
		# be far from the end of the log, so a bare tail would show only the
		# trailing noise; the tail is still useful for the surrounding context.
		_errors=$(grep -c -E 'FAILED|error:' "$_log" 2>/dev/null || true)
		printf '\n--- %s: %s error line(s) in %s ---\n' \
			"$_name" "${_errors:-0}" "$_log" >&2
		grep -n -E 'FAILED|error:' "$_log" 2>/dev/null | head -n 40 >&2 || true
		# Context around the failures, without ninja's echoed compiler command
		# lines: those are hundreds of characters wide and bury the diagnostics.
		printf '\n--- %s: context from %s ---\n' "$_name" "$_log" >&2
		grep -v -E '^(/usr/bin/|/usr/sbin/)?(ccache |cc |c\+\+ |gcc )' "$_log" \
			| tail -n 20 >&2 || true
		printf -- '--- end of %s output ---\n\n' "$_name" >&2
		return 1
	fi
	return 0
}

build_dep() {
	dir=$1
	name=$2
	log=$dir/build/$name.log
	if [ ! -f "$dir/build/build.ninja" ]; then
		echo "==> configuring $name"
		run_quiet "$name configure" "$dir/build/$name-configure.log" \
			meson setup "$dir/build" "$dir" \
			|| die "meson setup failed for $name (see $dir/build/$name-configure.log)"
	fi
	echo "==> building $name"
	run_quiet "$name build" "$log" ninja -C "$dir/build" \
		|| die "build failed for $name (see $log)"
}

# --- point pkg-config at the uninstalled trees ------------------------------
#
# This has to happen BEFORE the dependencies are configured. scenefx looks for
# the fork's wlroots-vkfx-<version> pkg-config name and falls back to a system
# wlroots-<version> when it cannot find it.

PKG_CONFIG_PATH="$WLR_DIR/build/meson-uninstalled:$SFX_DIR/build/meson-uninstalled${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PKG_CONFIG_PATH

build_dep "$WLR_DIR" wlroots
build_dep "$SFX_DIR" scenefx

# --- check the dependencies really did pick the fork ------------------------
#
# meson caches its dependency choices, so getting this wrong once poisons the
# build directory permanently and the symptom (effects silently missing at
# runtime, or a link against /usr/lib/libwlroots) shows up far away from the
# cause. Verify it where the choice is made.
# The generated .pc file is authoritative and reflects the cached choice.
assert_dep_uses_fork() {
	dir=$1
	name=$2
	pc=$(ls "$dir"/build/meson-uninstalled/*.pc 2>/dev/null | head -n1)
	[ -n "$pc" ] || return 0
	if grep -q 'wlroots-0.20' "$pc" &&
			! grep -q 'wlroots-vkfx-0.20' "$pc"; then
		die "$name was configured without the wlroots fork: it looked for
wlroots-vkfx-0.20, did not find it at $WLR_DIR/build/meson-uninstalled, and
fell back to a system wlroots. Build the fork first, or point WLR_DIR at the
tree that has build/meson-uninstalled. If $name was already configured, wipe
its build directory: meson caches the choice.
Offending pkg-config file: $pc"
	fi
}
assert_dep_uses_fork "$SFX_DIR" scenefx

# --- configure and build MangoWM -------------------------------------------

if [ "$reconf" = 1 ] && [ -f "$BUILD_DIR/build.ninja" ]; then
	echo "==> reconfiguring mango"
	meson configure "$BUILD_DIR" --wipe >/dev/null 2>&1 || true
fi

# meson records PKG_CONFIG_PATH as the pkg_config_path option when it first
# configures a build directory, and replays that recorded value on every later
# reconfigure, ignoring whatever the variable is set to now. It also caches the
# result of a dependency lookup for good. Neither a --reconfigure nor setting
# PKG_CONFIG_PATH again clears it. Only a wipe does.
#
# That is unfixable from outside the directory, but it is diagnosable.
if [ -f "$BUILD_DIR/meson-info/intro-buildoptions.json" ]; then
	recorded=$(grep -A3 '"name": "pkg_config_path"' \
		"$BUILD_DIR/meson-info/intro-buildoptions.json" \
		| grep -o '"/[^"]*"' | head -n1 | tr -d '"')
	case ${recorded:-} in
	*meson-uninstalled*) ;;
	*) printf 'warning: %s was configured with pkg_config_path=%s\n' \
		"$BUILD_DIR" "${recorded:-<empty>}" >&2
	   printf '         meson replays that recorded value instead of the current\n' >&2
	   printf '         PKG_CONFIG_PATH, so it may not see the trees above.\n' >&2
	   printf '         Re-run with --reconf to wipe and reconfigure.\n' >&2 ;;
	esac
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
	echo "==> configuring mango"
	run_quiet mango-configure "$BUILD_DIR.log" \
		meson setup "$BUILD_DIR" "$repo" --buildtype=debugoptimized \
		|| die "meson setup failed, see $BUILD_DIR.log"
else
	echo "==> configuring mango (incremental)"
	run_quiet mango-configure "$BUILD_DIR.log" \
		meson setup --reconfigure "$BUILD_DIR" "$repo" \
			--buildtype=debugoptimized \
		|| die "meson reconfigure failed, see $BUILD_DIR.log"
fi

# Confirm the intended trees were actually picked, rather than a system copy.
# This is a warning, not an error, because a wlroots that is genuinely installed
# from the fork is perfectly usable - but silently building against a different
# wlroots than the one named here is how mismatched headers produce errors that
# look like compiler bugs.
wlr_prefix=$(pkg-config --variable=libdir wlroots-vkfx-0.20 2>/dev/null || true)
case $wlr_prefix in
"$WLR_DIR"/build*) ;;
*) printf 'warning: wlroots-vkfx-0.20 did not resolve to %s/build\n' \
	"$WLR_DIR" >&2
   printf '         got: %s\n' "${wlr_prefix:-<not found>}" >&2 ;;
esac

# The same for the actual include flags the build will use, which is what decides
# whether the compiler sees the fork's headers at all.
if ! pkg-config --cflags wlroots-vkfx-0.20 >/dev/null 2>&1; then
	die "pkg-config cannot find wlroots-vkfx-0.20
Looked at $WLR_DIR/build/meson-uninstalled. Build the fork there first
(ninja -C $WLR_DIR/build), or set WLR_DIR to the right tree."
fi

echo "==> building mango"
run_quiet mango-build "$BUILD_DIR/mango.log" ninja -C "$BUILD_DIR" \
	|| die "build failed for mango (see $BUILD_DIR/mango.log)"

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
