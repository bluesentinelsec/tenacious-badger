"""Stage, archive, extract, and smoke-test native Release exports (stdlib only)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(arguments, **options):
    try:
        return subprocess.run([str(value) for value in arguments], check=True,
                              text=True, capture_output=True, timeout=60, **options).stdout
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"Native tool failed: {error.cmd}\n{error.stdout}{error.stderr}") from error


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def verify_windows_signatures(folder, publisher):
    certificate = folder / 'self-signing.cer'
    description = folder / 'signing.json'
    require(certificate.is_file() and description.is_file(),
            'Windows exports require signing metadata; enable TB_WINDOWS_SELF_SIGN')
    signing = json.loads(description.read_text(encoding='utf-8-sig'))
    require(signing['publisher'] == publisher, 'Unexpected signing metadata publisher')
    with certificate.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    require(digest.lower() == signing['certificate_sha256'].lower(), 'Signing certificate fingerprint mismatch')
    binaries = [path for path in folder.rglob('*') if path.suffix.lower() in ['.exe', '.dll', '.pyd']]
    require(binaries, 'No Windows binaries found for signature verification')
    with tempfile.TemporaryDirectory(prefix='tb-signatures-') as temporary:
        manifest = Path(temporary) / 'binaries.txt'
        manifest.write_text('\n'.join(str(path.resolve()) for path in binaries) + '\n', encoding='utf-8-sig')
        environment = os.environ.copy()
        environment.pop('PSModulePath', None)
        run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
             '-File', ROOT / 'scripts/windows/Sign-Binary.ps1', '-Action', 'VerifyManifest',
             '-FilePath', manifest, '-CertificateFile', certificate], env=environment)
    return signing


def metadata_check(folder, metadata, system):
    version = metadata['version']
    publisher = metadata['publisher']
    identifier = metadata['identifier']
    require((folder / 'LICENSE').is_file(), 'Missing Apache-2.0 license')
    require(publisher in (folder / 'NOTICE').read_text(), 'Missing personal attribution')
    if system == 'Windows':
        verify_windows_signatures(folder, publisher)
        executable = folder / 'burrow.exe'
        environment = os.environ.copy()
        environment['TB_EXPORTED_EXE'] = str(executable)
        environment['TB_EXPECTED_VERSION'] = version
        environment['TB_EXPECTED_PUBLISHER'] = publisher
        command = "$info = (Get-Item $env:TB_EXPORTED_EXE).VersionInfo; if ($info.CompanyName -ne $env:TB_EXPECTED_PUBLISHER -or $info.ProductName -ne 'Tenacious Badger' -or $info.ProductVersion -ne $env:TB_EXPECTED_VERSION -or $info.FileDescription -ne 'Burrow') { throw 'Incorrect native executable metadata' }"
        run(['powershell', '-NoProfile', '-Command', command], env=environment)
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        installation = Path(run([vswhere, '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                 '-property', 'installationPath']).strip())
        tools_version = (installation / 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt').read_text().strip()
        dumpbin = installation / 'VC/Tools/MSVC' / tools_version / 'bin/Hostx64/x64/dumpbin.exe'
        for binary in [executable, folder / 'badger.dll']:
            imports = run([dumpbin, '/dependents', binary]).lower()
            require(not any(name in imports for name in ['vcruntime', 'msvcp', 'asan', 'ubsan']),
                    f'Unexpected redistributable or sanitizer dependency in {binary.name}')
    elif system == 'Darwin':
        bundle = folder / 'Burrow.app'
        executable = bundle / 'Contents/MacOS/burrow'
        with (bundle / 'Contents/Info.plist').open('rb') as stream:
            info = plistlib.load(stream)
        require(info['CFBundleIdentifier'] == identifier, 'Incorrect bundle identifier')
        require(info['CFBundleShortVersionString'] == version, 'Incorrect bundle version')
        require(info['CFBundleVersion'] == version, 'Incorrect bundle build version')
        require(publisher in info['NSHumanReadableCopyright'], 'Incorrect bundle publisher')
        require((bundle / 'Contents/Resources/burrow.icns').is_file(), 'Missing macOS icon')
        require((bundle / 'Contents/Frameworks/libbadger.dylib').is_file(), 'Missing bundled engine')
        run(['codesign', '--verify', '--deep', '--strict', bundle])
        load_commands = run(['otool', '-l', executable])
        require('@executable_path/../Frameworks' in load_commands, 'Missing relative bundle runtime path')
        imports = run(['otool', '-L', executable]).lower()
        require('asan' not in imports and 'ubsan' not in imports, 'Sanitizer in release bundle')
    else:
        executable = folder / 'burrow'
        desktop = folder / 'share/applications' / f'{identifier}.desktop'
        require('Terminal=true' in desktop.read_text(), 'Incorrect bootstrap desktop entry')
        tree = ET.parse(folder / 'share/metainfo' / f'{identifier}.metainfo.xml')
        require(tree.findtext('id') == identifier, 'Incorrect AppStream identifier')
        require(tree.findtext('developer/name') == publisher, 'Incorrect AppStream publisher')
        require(tree.findtext('project_license') == 'Apache-2.0', 'Incorrect project license')
        require((folder / 'share/icons/hicolor/256x256/apps' / f'{identifier}.png').is_file(),
                'Missing Linux application icon')
        imports = run(['ldd', executable]).lower()
        require('not found' not in imports, 'Missing exported runtime library')
        require('asan' not in imports and 'ubsan' not in imports, 'Sanitizer in release archive')
        require(str(folder.resolve()).lower() in imports, 'Engine loaded outside the export')
    environment = os.environ.copy()
    for key in ['LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'DYLD_FALLBACK_LIBRARY_PATH']:
        environment.pop(key, None)
    output = run([executable], cwd=folder, env=environment)
    require(output.strip() == f'Tenacious Badger {version}: 2 + 3 = 5', 'Export smoke test failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('dist'))
    arguments = parser.parse_args()
    build = arguments.build.resolve()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    metadata = json.loads((build / 'desktop-metadata.json').read_text())
    require(metadata['version'] == (ROOT / 'VERSION').read_text().strip(), 'Build version is stale')
    system = platform.system()
    require(metadata['system'] == system, 'Only native exports are currently supported')
    cache = (build / 'CMakeCache.txt').read_text()
    for line in cache.splitlines():
        if line.startswith('CMAKE_BUILD_TYPE:STRING='):
            require(line.split('=', 1)[1] in ['', 'Release'], 'Export a Release build')
    architecture = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'arm64', 'aarch64': 'arm64'}[platform.machine().lower()]
    operating_system = {'Windows': 'windows', 'Darwin': 'macos', 'Linux': 'linux'}[system]
    name = f"tenacious-badger-{metadata['version']}-{operating_system}-{architecture}"
    with tempfile.TemporaryDirectory(prefix='tb-export-') as staging:
        folder = Path(staging) / name
        folder.mkdir()
        if system == 'Windows':
            for binary in ['burrow.exe', 'badger.dll']:
                shutil.copy2(build / 'bin' / binary, folder / binary)
            for document in ['self-signing.cer', 'signing.json']:
                shutil.copy2(build / 'signing' / document, folder / document)
            metadata['signing'] = json.loads((folder / 'signing.json').read_text(encoding='utf-8-sig'))
            metadata['minimum_runtime'] = 'Windows 10 or newer, x86_64; standard C runtime statically linked'
            launch = 'Run burrow.exe from a terminal. Windows binaries are signed with a local self-created certificate; self-signing.cer is the public certificate.'
        elif system == 'Darwin':
            bundle = folder / 'Burrow.app'
            shutil.copytree(build / 'bin/burrow.app', bundle, symlinks=True)
            for library in (bundle / 'Contents/Frameworks').glob('*.dylib'):
                if not library.is_symlink():
                    run(['codesign', '--force', '--sign', '-', '--timestamp=none', library])
            run(['codesign', '--force', '--sign', '-', '--timestamp=none', bundle])
            metadata['minimum_runtime'] = 'macOS 13 or newer, Apple silicon'
            metadata['signing'] = 'Ad-hoc signed; not Developer ID signed or notarized'
            launch = 'Run Burrow.app/Contents/MacOS/burrow from Terminal. The app is ad-hoc signed, not notarized.'
        else:
            shutil.copy2(build / 'bin/burrow', folder / 'burrow')
            shutil.copytree(build / 'bin/lib', folder / 'lib', symlinks=True)
            identifier = metadata['identifier']
            for category, extension in [('applications', 'desktop'), ('metainfo', 'metainfo.xml')]:
                destination = folder / 'share' / category
                destination.mkdir(parents=True)
                shutil.copy2(build / 'generated' / f'{identifier}.{extension}', destination)
            for size, extension in [('256x256', 'png'), ('scalable', 'svg')]:
                destination = folder / f'share/icons/hicolor/{size}/apps'
                destination.mkdir(parents=True)
                shutil.copy2(ROOT / 'resources/icons' / f'burrow.{extension}', destination / f'{identifier}.{extension}')
            metadata['minimum_runtime'] = f"Linux with {platform.libc_ver()[0]} {platform.libc_ver()[1]} or newer ({architecture})"
            launch = 'Run ./burrow from a terminal. Desktop metadata is in share/ for optional desktop integration.'
        for document in ['LICENSE', 'NOTICE']:
            shutil.copy2(ROOT / document, folder / document)
        (folder / 'README.txt').write_text(f"Tenacious Badger — under construction\n\n{launch}\n\nThis bootstrap prints 2 + 3 = 5 and exits. No game or renderer is implemented yet.\nPublisher: {metadata['publisher']}\nLicense: Apache-2.0\n", encoding='utf-8')
        (folder / 'BUILD-INFO.json').write_text(json.dumps(metadata, indent=2) + '\n')
        metadata_check(folder, metadata, system)
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
        with tempfile.TemporaryDirectory(prefix='tb-extract-') as extracted:
            if system == 'Darwin':
                run(['ditto', '-x', '-k', archive, extracted])
            elif system == 'Linux':
                with tarfile.open(archive) as stream:
                    stream.extractall(extracted, filter='data')
            else:
                with zipfile.ZipFile(archive) as stream:
                    stream.extractall(extracted)
            metadata_check(Path(extracted) / name, metadata, system)
    with archive.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    archive.with_name(archive.name + '.sha256').write_text(f'{digest}  {archive.name}\n')
    print(f'Validated native export: {archive.name}\nSHA-256: {digest}')


if __name__ == '__main__':
    main()
