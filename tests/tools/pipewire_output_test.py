"""Validate live PipeWire playback with GCC sanitizers and project allocation checks."""
import os
from pathlib import Path
import subprocess
import sys


def main(compiler, public_tests, internal_tests, probe):
    # GCC's ASan must be first. Keeping libdbus loaded preserves its global
    # roots when PipeWire unloads its client modules during context teardown.
    asan = subprocess.check_output([compiler, "-print-file-name=libasan.so"], text=True).strip()
    dbus = subprocess.check_output([compiler, "-print-file-name=libdbus-1.so.3"], text=True).strip()
    if not all(Path(library).is_absolute() and Path(library).is_file() for library in (asan, dbus)):
        raise SystemExit("The GCC ASan runtime and the system libdbus-1.so.3 must be installed")
    env = dict(os.environ, LD_PRELOAD=f"{asan}:{dbus}", ASAN_OPTIONS="detect_leaks=1",
               LSAN_OPTIONS="exitcode=23", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    # This existing probe leaks through tb_malloc and tb_calloc. The engine
    # counts the lost blocks, and LSan must independently report the bytes.
    control = subprocess.run([probe, "support_probe.leaked_block_fails_the_case"], env=env,
                             capture_output=True, text=True, timeout=30)
    report = control.stdout + control.stderr
    if (control.returncode != 23 or "memory leak: 2 blocks" not in report or
            "LeakSanitizer: detected memory leaks" not in report or
            "32 byte(s) leaked in 2 allocation(s)" not in report):
        raise SystemExit(f"Leak control failed to detect the two project allocations:\n{report}")
    print("The engine counted two missing blocks and LeakSanitizer reported both.", flush=True)

    cases = [(internal_tests, "platform_audio_linux.pipewire_plays"),
             (public_tests, "platform_audio_device.the_system_lists_its_outputs"),
             (public_tests, "platform_audio_device.the_default_output_asks_for_samples"),
             (public_tests, "platform_audio_device.the_device_s_own_format_and_unknown_devices")]
    for executable, case in cases:
        done = subprocess.run([executable, case], env=env, timeout=30)
        if done.returncode == 77:
            raise SystemExit(f"{case} skipped. Check the PipeWire socket, configuration and output.")
        if done.returncode != 0:
            raise SystemExit(f"{case} exited with status {done.returncode}")


if __name__ == "__main__":
    main(*sys.argv[1:])
