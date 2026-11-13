"""Archive and verify the installed dynamic/static C17 SDK, with no packages."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import zipfile

from package_desktop import ROOT, require, run


def verify(prefix, metadata):
    for header in ['api.h', 'core.h', 'platform.h', 'version.h']:
        require((prefix / 'include/badger' / header).is_file(), f'Missing public header: {header}')
    require((prefix / 'lib/cmake/TenaciousBadger/TenaciousBadgerConfig.cmake').is_file(),
            'Missing relocatable CMake package')
    static_name = 'badger_static.lib' if platform.system() == 'Windows' else 'libbadger_static.a'
    require((prefix / 'lib' / static_name).is_file(), 'Missing static engine library')
    require(metadata['publisher'] in (prefix / 'NOTICE').read_text(), 'Missing personal attribution')
    run([shutil.which('python3') or shutil.which('python'), ROOT / 'tests/ffi/ctypes_smoke.py',
         '--prefix', prefix])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    parser.add_argument('--configuration', choices=['Debug', 'Release'], required=True)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    metadata = json.loads((arguments.build / 'desktop-metadata.json').read_text())
    require(metadata['version'] == (ROOT / 'VERSION').read_text().strip(), 'Stale SDK version')
    metadata['configuration'] = arguments.configuration
    metadata['outputs'] = ['dynamic library', 'static library', 'public headers', 'CMake package']
    system = platform.system()
    operating_system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}[system]
    architecture = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}[platform.machine().lower()]
    name = f"tenacious-badger-sdk-{metadata['version']}-{operating_system}-{architecture}-{arguments.configuration.lower()}"
    with tempfile.TemporaryDirectory(prefix='tb-sdk-') as staging:
        folder = Path(staging) / name
        shutil.copytree(arguments.prefix, folder, symlinks=True)
        if system == 'Darwin' and arguments.configuration == 'Debug':
            run(['dsymutil', folder / 'lib/libbadger.dylib', '-o', folder / 'lib/libbadger.dylib.dSYM'])
        (folder / 'BUILD-INFO.json').write_text(json.dumps(metadata, indent=2) + '\n')
        (folder / 'README.txt').write_text(
            'Tenacious Badger C17 SDK — under construction\n\n'
            'Use find_package(TenaciousBadger CONFIG REQUIRED) with CMAKE_PREFIX_PATH set to this directory.\n'
            'Link TenaciousBadger::badger for the dynamic library, or TenaciousBadger::badger_static for the static library.\n'
            'Headers are under include/badger. Select the matching Debug or Release SDK.\n'
            'Windows static consumers must use the matching /MT or /MTd runtime; TB_STATIC is supplied by the CMake target.\n'
            'Publisher: bluesentinelsec\nLicense: Apache-2.0\n', encoding='utf-8')
        verify(folder, metadata)
        if system == 'Linux':
            archive = output / f'{name}.tar.gz'
            with tarfile.open(archive, 'w:gz') as stream:
                stream.add(folder, arcname=name)
        else:
            archive = output / f'{name}.zip'
            if system == 'Darwin':
                run(['ditto', '-c', '-k', '--keepParent', folder, archive])
            else:
                with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as stream:
                    for path in sorted(folder.rglob('*')):
                        if path.is_file():
                            stream.write(path, path.relative_to(folder.parent))
        with tempfile.TemporaryDirectory(prefix='tb-sdk-extract-') as extracted:
            if system == 'Darwin':
                run(['ditto', '-x', '-k', archive, extracted])
            elif system == 'Linux':
                with tarfile.open(archive) as stream:
                    stream.extractall(extracted, filter='data')
            else:
                with zipfile.ZipFile(archive) as stream:
                    stream.extractall(extracted)
            verify(Path(extracted) / name, metadata)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'Validated {arguments.configuration} dynamic/static SDK: {archive.name}')


if __name__ == '__main__':
    main()
