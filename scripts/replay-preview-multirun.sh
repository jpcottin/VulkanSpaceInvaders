#!/usr/bin/env bash
#
# Replay the CI job "Emulator Preview experiment multi-run" on this machine.
#
# The job boots the Android Emulator Preview (emulators;latest) several times
# WITHOUT -no-snapshot: each cycle plays the game for a few seconds, screenshots
# it, then shuts down gracefully so the emulator writes its quickboot snapshot.
# If restore preserves the running app, the app keeps the same pid across cycles
# and the game continues where it left off.
#
# Running it here instead of pushing turns a ~25 minute CI round trip into a few
# minutes, which matters when iterating on the emulator itself.
#
# Usage:
#   scripts/replay-preview-multirun.sh [-n RUNS] [-a AVD_NAME] [-o OUTDIR] [-k]
#
#   -n RUNS   number of boot/snapshot cycles (default 4)
#   -a NAME   AVD to create and use (default preview_replay)
#   -o DIR    output directory for screenshots and logs (default /tmp/preview-replay)
#   -k        keep the AVD afterwards (default: delete it -- it is several GB)
#
# Requirements: $ANDROID_HOME with the emulators;latest package and the
# system image below, plus KVM. Install with:
#   sdkmanager --channel=3 --install 'emulators;latest' \
#     'system-images;android-37.0;google_apis_ps16k;x86_64'
set -u

RUNS=4
AVD=preview_replay
OUT=/tmp/preview-replay
KEEP=0
while getopts ':n:a:o:kh' opt; do
  case "$opt" in
    n) RUNS="$OPTARG" ;;
    a) AVD="$OPTARG" ;;
    o) OUT="$OPTARG" ;;
    k) KEEP=1 ;;
    h) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "unknown option -$OPTARG (try -h)" >&2; exit 2 ;;
  esac
done

case "$AVD" in
  ''|*/*) echo "ERROR: -a needs a plain AVD name" >&2; exit 2 ;;
esac
case "$RUNS" in
  ''|*[!0-9]*|0) echo "ERROR: -n needs a positive number of cycles" >&2; exit 2 ;;
esac

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
export ANDROID_HOME="$SDK" ANDROID_SDK_ROOT="$SDK"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulators/latest/emulator"
API=37.0
TARGET=google_apis_ps16k
ABI=x86_64
PKG=com.jpcottin.vulkanspaceinvaders
REPO="$(cd "$(dirname "$0")/.." && pwd)"
APK="$REPO/app/build/outputs/apk/debug/app-debug.apk"

[ -x "$EMULATOR" ] || { echo "ERROR: no preview emulator at $EMULATOR"; exit 1; }
[ -x "$ADB" ] || { echo "ERROR: no adb at $ADB"; exit 1; }
[ -e /dev/kvm ] || echo "WARNING: /dev/kvm missing; the emulator will be extremely slow"

mkdir -p "$OUT/screenshots"
rm -f "$OUT"/screenshots/*.png "$OUT"/emulator_run*.txt "$OUT"/logcat_run*.txt "$OUT"/pids.txt

# This host may have other devices on adb (another emulator, a Cuttlefish
# instance, a phone). Every adb call is pinned to the emulator we launch, and
# process cleanup is scoped to its own process group -- never a pattern match
# across all emulator processes.
SERIAL=""
A() { "$ADB" -s "$SERIAL" "$@"; }
SELF_PGID="$(ps -o pgid= -p $$ | tr -d ' ')"

banner() { echo; echo "################ $* ################"; }

# The emulator console starts unauthenticated, where it offers only
# help/ping/auth/quit/avd -- 'kill' and 'avd pause' are not available, so
# 'adb emu kill' returns "KO: unknown command". The emulator reads this token
# file but does not create it, so create it if absent. An existing token is
# left alone.
TOKEN="$HOME/.emulator_console_auth_token"
if [ ! -s "$TOKEN" ]; then
  printf 'replayConsoleToken' > "$TOKEN"
  chmod 600 "$TOKEN"
  echo "created $TOKEN"
fi

banner "SETUP: AVD $AVD"
# Only an AVD this script created may be deleted at the end -- never one that
# was already on the machine, even if -a named it explicitly.
CREATED_AVD=0
if [ -d "$HOME/.android/avd/$AVD.avd" ] && [ -f "$HOME/.android/avd/$AVD.ini" ]; then
  echo "reusing the existing AVD $AVD (it will be left in place)"
elif [ -d "$HOME/.android/avd/$AVD.avd" ] || [ -f "$HOME/.android/avd/$AVD.ini" ]; then
  # A directory without its .ini (or vice versa) is not a usable AVD -- an
  # emulator that was still shutting down when a previous run cleaned up can
  # leave one behind. Replace it rather than trying to boot it.
  echo "found an incomplete AVD $AVD (missing its .ini or .avd); replacing it"
  rm -rf "$HOME/.android/avd/$AVD.avd" "$HOME/.android/avd/$AVD.ini"
  CREATED_AVD=1
else
  CREATED_AVD=1
fi
if [ ! -d "$HOME/.android/avd/$AVD.avd" ]; then
  # avdmanager silently produces nothing once emulators;latest is installed,
  # so write the AVD files directly. This mirrors what the CI job does.
  echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd --force -n "$AVD" \
    --abi "$TARGET/$ABI" --device 'pixel_6' \
    --package "system-images;android-$API;$TARGET;$ABI" >/dev/null 2>&1 || true
fi
if [ ! -d "$HOME/.android/avd/$AVD.avd" ]; then
  echo "avdmanager produced nothing; writing the AVD files by hand"
  mkdir -p "$HOME/.android/avd/$AVD.avd"
  printf 'avd.ini.encoding=UTF-8\npath=%s/.android/avd/%s.avd\npath.rel=avd/%s.avd\ntarget=android-%s\n' \
    "$HOME" "$AVD" "$AVD" "$API" > "$HOME/.android/avd/$AVD.ini"
  cat > "$HOME/.android/avd/$AVD.avd/config.ini" << CFG
AvdId=$AVD
avd.ini.displayname=$AVD
avd.ini.encoding=UTF-8
abi.type=$ABI
hw.cpu.arch=$ABI
image.sysdir.1=system-images/android-$API/$TARGET/$ABI/
tag.id=google_apis
tag.display=Google APIs
PlayStore.enabled=no
hw.lcd.density=420
hw.lcd.width=1080
hw.lcd.height=2400
hw.keyboard=yes
hw.gpu.enabled=yes
hw.gpu.mode=auto
disk.dataPartition.size=8192M
hw.ramSize=4096
hw.cpu.ncore=4
CFG
fi

if [ ! -f "$APK" ]; then
  banner "Building the debug APK"
  (cd "$REPO" && ./gradlew assembleDebug --no-daemon) || exit 1
fi

cleanup() {
  # On an interrupted run, take down the emulator we started -- by process
  # group when we managed to isolate one, otherwise by pid.
  if [ -n "${EMU_PGID:-}" ] && kill -0 -- "-$EMU_PGID" 2>/dev/null; then
    kill -TERM -- "-$EMU_PGID" 2>/dev/null || true
  elif [ -n "${EMU_PID:-}" ] && kill -0 "$EMU_PID" 2>/dev/null; then
    kill -TERM "$EMU_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

boot_and_play() {
  N="$1"
  banner "RUN $N: launching emulator (snapshots enabled)"
  # Record the emulators already attached, so we can identify ours as the one
  # that appears afterwards. Never assume emulator-5554 is ours: this machine
  # may already be running an emulator on the default ports.
  DEVICES_BEFORE="$("$ADB" devices | awk '/^emulator-/{print $1}' | sort)"
  # No -no-snapshot: load the quickboot snapshot if present, save it on exit.
  setsid "$EMULATOR" @"$AVD" -no-window -gpu auto -noaudio -no-boot-anim \
    -camera-back none -memory 4096 -verbose -show-kernel \
    > "$OUT/emulator_run$N.txt" 2>&1 &
  EMU_PID=$!
  # setsid(2) happens asynchronously, so poll until the process group settles.
  EMU_PGID=""
  for _ in $(seq 1 20); do
    P="$(ps -o pgid= -p "$EMU_PID" 2>/dev/null | tr -d ' ')"
    if [ -n "$P" ] && [ "$P" != "$SELF_PGID" ]; then EMU_PGID="$P"; break; fi
    kill -0 "$EMU_PID" 2>/dev/null || break
    sleep 0.5
  done
  [ -n "$EMU_PGID" ] || echo "WARNING: could not isolate a process group; using single-pid shutdown"

  SERIAL=""
  for _ in $(seq 1 36); do
    kill -0 "$EMU_PID" 2>/dev/null || { echo "ERROR: emulator exited early"; tail -30 "$OUT/emulator_run$N.txt"; return 1; }
    cand="$(comm -13 <(printf '%s\n' "$DEVICES_BEFORE") \
                     <("$ADB" devices | awk '/^emulator-/{print $1}' | sort) | head -1)"
    [ -n "$cand" ] && { SERIAL="$cand"; break; }
    sleep 5
  done
  if [ -z "$SERIAL" ]; then
    echo "ERROR: the emulator we launched never appeared on adb after 180s"
    if grep -q "address already in use" "$OUT/emulator_run$N.txt" 2>/dev/null; then
      echo "       its console port is taken -- another emulator is already running:"
      "$ADB" devices | sed 's/^/         /'
      echo "       stop it, or free the console ports, and try again."
    fi
    return 1
  fi
  echo "RUN $N serial=$SERIAL pid=$EMU_PID pgid=${EMU_PGID:-none}"

  for _ in $(seq 1 48); do
    [ "$(A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && break
    sleep 10
  done
  [ "$(A shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] || { echo "ERROR: boot timeout"; return 1; }
  T_RESUME=$(date +%s)

  if [ "$N" = 1 ]; then
    # A guest error dialog would block all input and be preserved by the
    # snapshot, so suppress dialogs for the duration of the replay.
    A shell settings put global hide_error_dialogs 1 >/dev/null || true
    A install -r "$APK" >/dev/null || return 1
    # Auto-play must be on BEFORE the first start: the game reads
    # <files>/settings.bin at startup (magic "SETT" LE, soundOn=0, autoPlay=1).
    A shell "run-as $PKG sh -c 'mkdir -p files; echo VFRFUwAAAAABAAAA | base64 -d > files/settings.bin'"
    A shell input keyevent KEYCODE_WAKEUP >/dev/null || true
    A shell wm dismiss-keyguard >/dev/null || true
  else
    # The restored frame, before any input: should match the previous cycle's
    # exit screenshot, which was taken immediately before that cycle froze.
    A exec-out screencap -p > "$OUT/screenshots/run$N-entry.png" || true
  fi

  RUN_PID="$(A shell pidof "$PKG" 2>/dev/null | tr -d '\r')"
  echo "RUN $N app pid after boot/restore: ${RUN_PID:-none}"
  echo "cycle $N: ${RUN_PID:-none}" >> "$OUT/pids.txt"
  if [ "$N" = 1 ]; then
    A shell am start -n "$PKG/android.app.NativeActivity" >/dev/null 2>&1 || true
  elif [ -z "$RUN_PID" ]; then
    # Deliberately NOT relaunched, same as the CI job this replays: the
    # question is whether the snapshot brings the app back by itself, and a
    # relaunch here would answer it for the emulator.
    echo "RUN $N app survived restore: NO (process gone; left as-is)"
  fi
  if [ "$N" = 1 ] || [ -n "$RUN_PID" ]; then
    for _ in $(seq 1 30); do
      A shell dumpsys window 2>/dev/null | grep -qi "ocus.*vulkanspaceinvaders" && break
      sleep 1
    done
  fi
  sleep 3
  # The TITLE screen needs a tap to start a round; auto-play only flies during
  # PLAYING. Taps are spaced out because input to an unfocused window is dropped.
  for _ in 1 2 3 4; do A shell input tap 540 1500 >/dev/null || true; sleep 2; done
  sleep 5

  A logcat -d -t 500 > "$OUT/logcat_run$N.txt" 2>/dev/null || true
  # Liveness: two captures 2s apart must differ, or rendering has stalled.
  A exec-out screencap -p > "$OUT/liveness-a.png" || true
  sleep 2
  A exec-out screencap -p > "$OUT/liveness-b.png" || true
  if cmp -s "$OUT/liveness-a.png" "$OUT/liveness-b.png"; then
    echo "RUN $N display: FROZEN (two captures 2s apart are identical)"
  else
    echo "RUN $N display: LIVE"
  fi

  # Exit screenshot last, immediately before shutdown, so it is as close as
  # possible to the state the snapshot captures. A few seconds still elapse
  # while the emulator shuts down and the guest keeps running, so the next
  # cycle's entry screenshot is close to this one rather than identical.
  A exec-out screencap -p > "$OUT/screenshots/run$N.png" || true
  echo "RUN $N app pid before shutdown: $(A shell pidof "$PKG" 2>/dev/null | tr -d '\r' || echo none)"

  T_KILL=$(date +%s)
  echo "RUN $N: shutting down via adb emu kill"
  A emu kill >/dev/null 2>&1 || true
  for _ in $(seq 1 90); do kill -0 "$EMU_PID" 2>/dev/null || break; sleep 1; done
  if kill -0 "$EMU_PID" 2>/dev/null; then
    echo "WARNING: still up 90s after 'adb emu kill'; terminating"
    if [ -n "$EMU_PGID" ]; then kill -TERM -- "-$EMU_PGID" 2>/dev/null || true
    else kill "$EMU_PID" 2>/dev/null || true; fi
    sleep 10
  fi
  # Forget the process group only once it is really gone: the end-of-script
  # guard relies on EMU_PGID to keep a still-running emulator's AVD alive.
  if [ -n "$EMU_PGID" ]; then
    for _ in $(seq 1 120); do kill -0 -- "-$EMU_PGID" 2>/dev/null || break; sleep 1; done
    if kill -0 -- "-$EMU_PGID" 2>/dev/null; then
      echo "WARNING: emulator group still alive 120s after TERM; sending KILL"
      kill -KILL -- "-$EMU_PGID" 2>/dev/null || true
      sleep 3
    fi
    kill -0 -- "-$EMU_PGID" 2>/dev/null || EMU_PGID=""
  fi
  echo "RUN $N: down $(( $(date +%s) - T_KILL ))s after the shutdown request (played ~$(( T_KILL - T_RESUME ))s)"
  LD_LIBRARY_PATH="$SDK/emulators/latest/lib64" \
    "$SDK/emulators/latest/bin/qemu-img" snapshot -l \
    "$HOME/.android/avd/$AVD.avd/userdata-qemu.img.qcow2" 2>&1 | sed 's/^/  /' || true
  sleep 3
}

for n in $(seq 1 "$RUNS"); do
  boot_and_play "$n" || { echo "run $n failed"; break; }
done

# A cycle that failed part-way may leave the emulator running. Take it down and
# wait for it before reporting or removing the AVD: an emulator still shutting
# down will recreate the directory underneath us.
cleanup
if [ -n "${EMU_PGID:-}" ]; then
  for _ in $(seq 1 60); do kill -0 -- "-$EMU_PGID" 2>/dev/null || break; sleep 1; done
  kill -0 -- "-$EMU_PGID" 2>/dev/null && echo "WARNING: the emulator is still running; leaving the AVD alone" && KEEP=1
fi

banner "RESULT"
echo "app pid per cycle (identical from cycle 2 on = the app survived every restore):"
sed 's/^/  /' "$OUT/pids.txt" 2>/dev/null || true
for n in $(seq 2 "$RUNS"); do
  p=$((n-1))
  if [ -f "$OUT/screenshots/run$n-entry.png" ] && [ -f "$OUT/screenshots/run$p.png" ]; then
    if cmp -s "$OUT/screenshots/run$n-entry.png" "$OUT/screenshots/run$p.png"; then
      echo "  run$n-entry == run$p exit  (identical: the snapshot froze exactly at the screenshot)"
    else
      echo "  run$n-entry != run$p exit  (differs: the guest advanced between capture and freeze)"
    fi
  fi
done
echo
echo "screenshots and logs: $OUT"
if [ "$KEEP" = 0 ] && [ "$CREATED_AVD" = 1 ]; then
  rm -rf "$HOME/.android/avd/$AVD.avd" "$HOME/.android/avd/$AVD.ini"
  echo "removed the AVD it created ($AVD); pass -k to keep it"
else
  echo "kept AVD $AVD ($(du -sh "$HOME/.android/avd/$AVD.avd" 2>/dev/null | cut -f1))"
fi
