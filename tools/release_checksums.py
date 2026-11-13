"""Verify all native archives and write the release's aggregate SHA-256 manifest."""
import hashlib
from pathlib import Path
import sys


def main():
    folder = Path(sys.argv[1])
    version = (Path(__file__).resolve().parents[1] / "VERSION").read_text().strip()
    platforms = {"windows-x86_64.zip", "macos-arm64.zip", "linux-x86_64.tar.gz"}
    expected = {f"tenacious-badger-{version}-{platform}" for platform in platforms}
    expected.update(f"tenacious-badger-sdk-{version}-{platform}-{configuration}{extension}"
                    for platform, extension in [("windows-x86_64", ".zip"), ("macos-arm64", ".zip"), ("linux-x86_64", ".tar.gz")]
                    for configuration in ["debug", "release"])
    archives = {path.name: path for path in folder.iterdir() if path.name.endswith((".zip", ".tar.gz"))}
    if set(archives) != expected:
        raise RuntimeError(f"Expected exactly nine game/SDK exports, got {sorted(archives)}")
    lines = []
    for name, path in sorted(archives.items()):
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        line = f"{digest}  {name}\n"
        if path.with_name(name + ".sha256").read_text() != line:
            raise RuntimeError(f"Archive checksum mismatch: {name}")
        lines.append(line)
    (folder / "SHA256SUMS").write_text("".join(lines))
    print("Verified three desktop exports and six Debug/Release SDKs; wrote SHA256SUMS.")


if __name__ == "__main__":
    main()
