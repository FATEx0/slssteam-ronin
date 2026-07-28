#!/bin/sh
set -eu

usage()
{
	echo "usage: $0 [--rollback] <tsuki-root>" >&2
	exit 2
}

mode=install
if [ "$#" -eq 2 ] && [ "$1" = "--rollback" ]; then
	mode=rollback
	shift
fi
[ "$#" -eq 1 ] || usage

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
tsuki_root=$1
module_root=$tsuki_root/modules
target=$module_root/slsteam
rollback_root=$tsuki_root/.ronin-rollback
rollback=$rollback_root/slsteam
# ronin-module-sdk is its own project now, not nested under tsuki_root
# (moved 2026-07-28). Default to a sibling of tsuki_root; override with
# RONIN_SDK_ROOT if your checkout layout differs.
sdk_root=${RONIN_SDK_ROOT:-$(CDPATH= cd -- "$tsuki_root/.." && pwd)/ronin-module-sdk}
validator=$sdk_root/tools/ronin_validate.py

[ -f "$repo_root/module/module.json" ] || {
	echo "canonical package is missing: $repo_root/module" >&2
	exit 1
}
[ -d "$tsuki_root/lua" ] && [ -f "$tsuki_root/lua/roninregistry.lua" ] || {
	echo "not a Tsuki checkout: $tsuki_root" >&2
	exit 1
}
[ -f "$validator" ] || {
	echo "Ronin validator is missing: $validator (set RONIN_SDK_ROOT if ronin-module-sdk isn't a sibling of $tsuki_root)" >&2
	exit 1
}

mkdir -p "$module_root" "$rollback_root"

validate_package()
{
	package=$1
	if [ -n "${PYTHON:-}" ]; then
		"$PYTHON" "$validator" "$package"
	elif command -v python3 >/dev/null 2>&1; then
		python3 "$validator" "$package"
	elif command -v nix-shell >/dev/null 2>&1; then
		RONIN_VALIDATE_SCRIPT=$validator RONIN_VALIDATE_PACKAGE=$package \
			nix-shell -p python3 --run \
			'python3 "$RONIN_VALIDATE_SCRIPT" "$RONIN_VALIDATE_PACKAGE"'
	else
		echo "Python 3 is required to validate the Ronin package" >&2
		exit 1
	fi
}

if [ "$mode" = rollback ]; then
	[ -d "$rollback" ] || {
		echo "no rollback package is available: $rollback" >&2
		exit 1
	}
	validate_package "$rollback"
	current=$(mktemp -d "$module_root/.slsteam.current.XXXXXX")
	rmdir -- "$current"
	if [ -e "$target" ]; then
		mv -- "$target" "$current"
	fi
	if ! mv -- "$rollback" "$target"; then
		if [ ! -e "$target" ] && [ -e "$current" ]; then
			mv -- "$current" "$target"
		fi
		exit 1
	fi
	if [ -e "$current" ]; then
		mv -- "$current" "$rollback"
	fi
	echo "restored: $target"
	if [ -e "$rollback" ]; then
		echo "rollback: $rollback"
	fi
	exit 0
fi

stage=$(mktemp -d "$module_root/.slsteam.next.XXXXXX")
cleanup()
{
	if [ -n "${stage:-}" ] && [ -d "$stage" ]; then
		rm -rf -- "$stage"
	fi
}
trap cleanup EXIT HUP INT TERM

cp -a "$repo_root/module/." "$stage/"

validate_package "$stage"

if [ -e "$rollback" ]; then
	case "$rollback" in
		"$rollback_root"/slsteam) rm -rf -- "$rollback" ;;
		*) echo "refusing unexpected rollback path: $rollback" >&2; exit 1 ;;
	esac
fi

if [ -e "$target" ]; then
	mv -- "$target" "$rollback"
fi

if ! mv -- "$stage" "$target"; then
	if [ ! -e "$target" ] && [ -e "$rollback" ]; then
		mv -- "$rollback" "$target"
	fi
	exit 1
fi
stage=
trap - EXIT HUP INT TERM

echo "installed: $target"
if [ -e "$rollback" ]; then
	echo "rollback:  $rollback"
fi
