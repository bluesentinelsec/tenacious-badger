"""Build and run isolated C17 consumers of an installed foundation SDK."""
import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def verify_foundation(prefix, configuration):
    prefix = Path(prefix).resolve()
    scratch = None
    if platform.system() == 'Windows':
        # MSBuild warns about incremental builds under the system temp folder.
        # Keep this isolated, disposable build outside both it and the checkout.
        scratch = Path.home() / '.cache/tenacious-badger/consumer-checks'
        scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tb-foundation-consumer-', dir=scratch) as temporary:
        root = Path(temporary)
        source = root / 'source'
        build = root / 'build'
        shutil.copytree(ROOT / 'tests/consumers/foundation', source)
        command = ['cmake', '-S', str(source), '-B', str(build),
                   f'-DCMAKE_PREFIX_PATH={prefix}',
                   f'-DPython3_EXECUTABLE={sys.executable}']
        if platform.system() == 'Windows':
            # The consumer has no checkout-relative includes. Copy development
            # signing policy separately; it contributes no engine source/header.
            policy = root / 'policy'
            (policy / 'cmake').mkdir(parents=True)
            (policy / 'scripts/windows').mkdir(parents=True)
            shutil.copy2(ROOT / 'cmake/WindowsSigning.cmake', policy / 'cmake')
            shutil.copy2(ROOT / 'scripts/windows/Sign-Binary.ps1', policy / 'scripts/windows')
            command += ['-G', 'Visual Studio 18 2026', '-A', 'x64',
                        f'-DTB_CONSUMER_SIGNING_MODULE={policy / "cmake/WindowsSigning.cmake"}']
        else:
            command += [f'-DCMAKE_BUILD_TYPE={configuration}']
        subprocess.run(command, check=True)
        subprocess.run(['cmake', '--build', str(build), '--config', configuration, '--parallel'],
                       check=True)
        subprocess.run(['ctest', '--test-dir', str(build), '-C', configuration,
                        '--output-on-failure', '--no-tests=error'], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], required=True)
    args = parser.parse_args()
    verify_foundation(args.prefix, args.configuration)


if __name__ == '__main__':
    main()
