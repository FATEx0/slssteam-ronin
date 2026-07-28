#!/bin/sh
# Build the current slssteam-ronin working tree, deploy it into a fresh
# throwaway modules/ directory (never the production
# $tsuki_root/modules/slsteam), hard-kill any running Steam, and launch
# Tsuki against that fresh module using the real, already-logged-in Steam
# session ($HOME untouched -- no separate test account).
#
# Restart sequence mirrors lua/launch.lua's own managed-restart path
# (RESTART_KILL_SIGNAL / RESTART_QUIESCENCE_SECONDS): SIGKILL steam +
# steamwebhelper, not a graceful logoff -- a graceful exit leaves the
# relaunched client stuck at SharedJSContext, never calling LogOn() again,
# while a hard kill lets Steam's own crash recovery restore the
# authenticated session from the persisted login token. Real, hard-won
# operational knowledge (see that file's comment) -- reused here.
set -eu

usage()
{
	echo "usage: $0 [tsuki-root]" >&2
	echo "  tsuki-root defaults to /home/fatex0/Projects/tsuki" >&2
	exit 2
}

[ "$#" -le 1 ] || usage

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tsuki_root=${1:-/home/fatex0/Projects/tsuki}

[ -x "$tsuki_root/bin/tsuki" ] || {
	echo "no built Tsuki binary at $tsuki_root/bin/tsuki (build it first)" >&2
	exit 1
}
[ -f "$tsuki_root/lua/roninregistry.lua" ] || {
	echo "not a Tsuki checkout: $tsuki_root" >&2
	exit 1
}
# ronin-module-sdk is its own project now, not nested under tsuki_root
# (moved 2026-07-28). Default to a sibling of tsuki_root; override with
# RONIN_SDK_ROOT if your checkout layout differs.
sdk_root=${RONIN_SDK_ROOT:-$(CDPATH= cd -- "$tsuki_root/.." && pwd)/ronin-module-sdk}
validator="$sdk_root/tools/ronin_validate.py"
[ -f "$validator" ] || {
	echo "Ronin validator is missing: $validator (set RONIN_SDK_ROOT if ronin-module-sdk isn't a sibling of $tsuki_root)" >&2
	exit 1
}

echo "==> building latest slsteam module ($repo_root)"
( cd "$repo_root" && nix develop -c make ronin-module JOBS=2 )

stamp=$(date +%Y%m%d-%H%M%S)
root="$tsuki_root/.live-sls-test/$stamp"
mkdir -p "$root/modules"
cp -r "$repo_root/module" "$root/modules/slsteam"
echo "==> fresh module root: $root"

echo "==> validating package"
if command -v uv >/dev/null 2>&1; then
	UV_CACHE_DIR=${UV_CACHE_DIR:-/tmp/codex-uv-cache} \
		uv run python "$validator" "$root/modules/slsteam"
elif command -v nix-shell >/dev/null 2>&1; then
	RONIN_VALIDATE_SCRIPT=$validator RONIN_VALIDATE_PACKAGE="$root/modules/slsteam" \
		nix-shell -p python3 --run \
		'python3 "$RONIN_VALIDATE_SCRIPT" "$RONIN_VALIDATE_PACKAGE"'
else
	echo "uv or nix-shell is required to validate the Ronin package" >&2
	exit 1
fi

echo "==> killing any previous live-test Tsuki instance (and its ronin-control)"
# A leftover tsuki from an earlier run holds the module's control Unix
# socket (~/.local/share/Tsuki/module-sockets/slsteam.sock) -- the next
# run's spec module then refuses to steal it and silently fails to load
# the module wrapper (native SLSsteam.so injection still works fine, but
# Tsuki's own settings/log UI for it does not). Kill any prior instance
# before spawning a new one, every time.
pkill -9 -x ronin-control 2>/dev/null || true
pkill -9 -x tsuki 2>/dev/null || true

echo "==> hard-killing any running Steam (SIGKILL steam + steamwebhelper)"
pkill -9 -x steam 2>/dev/null || true
pkill -9 -x steamwebhelper 2>/dev/null || true

alive_by_comm()
{
	# pgrep alone also matches zombies (state Z) that can never be killed by
	# us (e.g. a defunct process reparented away from its original parent) --
	# a stray one of those would make this wait loop time out forever.
	# Exclude zombies so only an actually-still-running process counts.
	ps -eo stat,comm 2>/dev/null | awk -v name="$1" '$2 == name && $1 !~ /Z/ { found=1 } END { exit !found }'
}

echo "==> waiting for a clean exit"
tries=0
while alive_by_comm steam || alive_by_comm tsuki; do
	tries=$((tries + 1))
	if [ "$tries" -gt 30 ]; then
		echo "steam/tsuki did not exit after SIGKILL" >&2
		exit 1
	fi
	sleep 0.5
done
sleep 3 # RESTART_QUIESCENCE_SECONDS in launch.lua -- let killed socket/IPC state settle

echo "==> launching Tsuki against the fresh module (real Steam login, unchanged \$HOME)"
cd "$tsuki_root"
TSUKI_LUA_DIR="$tsuki_root/lua" \
TSUKI_RONIN_MODULE_DIR="$root/modules" \
nohup "$tsuki_root/bin/tsuki" >"$root/tsuki.log" 2>&1 &
tsuki_pid=$!
echo "$tsuki_pid" >"$root/tsuki.pid"
echo "==> Tsuki started, pid $tsuki_pid, log: $root/tsuki.log"

echo "==> waiting for Steam to come up"
tries=0
while ! grep -q "forked Steam as pid" "$root/tsuki.log" 2>/dev/null; do
	tries=$((tries + 1))
	if [ "$tries" -gt 60 ]; then
		echo "Steam did not report a pid within 30s -- check $root/tsuki.log" >&2
		exit 1
	fi
	sleep 0.5
done

grep -E "forked Steam as pid|supervising Steam|CEF (port|endpoint)" "$root/tsuki.log" | tail -5
echo
echo "==> live: module root=$root"
echo "==> tsuki pid file: $root/tsuki.pid"
echo "==> tsuki log: $root/tsuki.log"
