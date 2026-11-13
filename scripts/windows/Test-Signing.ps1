#Requires -Version 5.1
<# .SYNOPSIS
Checks certificate reuse and rejection of unsigned, modified, and wrong-signer PE files.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinaryPath,
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$SigningDirectory,
    [string]$Publisher = 'bluesentinelsec'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot 'Sign-Binary.ps1'
$powerShellPath = (Get-Process -Id $PID).Path
$publicCertificate = Join-Path $SigningDirectory 'self-signing.cer'
$expectedThumbprint = (Get-Content -LiteralPath (Join-Path $SigningDirectory 'thumbprint.txt') -Raw).Trim()
$tempDirectory = Join-Path ([IO.Path]::GetTempPath()) ("Badger signing checks " + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempDirectory
$overrideCertificate = $null

function Assert-VerificationResult {
    param([string]$Path, [string]$Thumbprint, [switch]$ShouldSucceed)
    $process = Start-Process -FilePath $powerShellPath -Wait -PassThru -NoNewWindow `
        -ArgumentList @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', ('"' + $scriptPath + '"'), '-Action', 'Verify',
            '-FilePath', ('"' + $Path + '"'), '-CertificateThumbprint', $Thumbprint) `
        -RedirectStandardOutput (Join-Path $tempDirectory 'negative-output.txt') `
        -RedirectStandardError (Join-Path $tempDirectory 'negative-error.txt')
    if (($process.ExitCode -eq 0) -ne $ShouldSucceed.IsPresent) {
        throw "Unexpected verification exit code $($process.ExitCode): $Path"
    }
}

try {
    & $scriptPath -Action Verify -FilePath $BinaryPath -CertificateFile $publicCertificate
    Assert-VerificationResult -Path $BinaryPath -Thumbprint $expectedThumbprint -ShouldSucceed
    & $scriptPath -Action Initialize -OutputDirectory $tempDirectory -Publisher $Publisher `
        -CertificateThumbprint $expectedThumbprint
    $reusedThumbprint = (Get-Content -LiteralPath (Join-Path $tempDirectory 'thumbprint.txt') -Raw).Trim()
    if ($reusedThumbprint -ne $expectedThumbprint) {
        throw 'Repeated initialization must reuse the signing identity.'
    }
    if (@(Get-ChildItem -LiteralPath $tempDirectory -Filter '*.pfx').Count -ne 0) {
        throw 'Initialization must never export a private key.'
    }

    # The compiler-identification executable is a real, unsigned PE image.
    $unsigned = Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'CMakeFiles') `
        -Filter '*.exe' -File -Recurse |
        Where-Object { (Get-AuthenticodeSignature -LiteralPath $_.FullName).Status -eq 'NotSigned' } |
        Select-Object -First 1
    if (-not $unsigned) {
        throw 'Missing unsigned compiler-identification fixture.'
    }
    Assert-VerificationResult -Path $unsigned.FullName -Thumbprint $expectedThumbprint
    Assert-VerificationResult -Path $BinaryPath -Thumbprint ('0' * 40)

    $tampered = Join-Path $tempDirectory 'tampered binary.exe'
    $bytes = [IO.File]::ReadAllBytes($BinaryPath)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    $optionalHeaderLength = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $firstSection = $peOffset + 24 + $optionalHeaderLength
    $rawDataOffset = [BitConverter]::ToUInt32($bytes, $firstSection + 20)
    if ($rawDataOffset -eq 0 -or $rawDataOffset -ge $bytes.Length) {
        throw 'Cannot locate signed PE section data for the tamper check.'
    }
    $bytes[$rawDataOffset] = $bytes[$rawDataOffset] -bxor 1
    [IO.File]::WriteAllBytes($tampered, $bytes)
    Assert-VerificationResult -Path $tampered -Thumbprint $expectedThumbprint

    # A supplied certificate keeps its own identity instead of being forced to
    # match the locally generated certificate's CN or friendly name.
    $overrideCertificate = New-SelfSignedCertificate -Type CodeSigningCert `
        -Subject 'CN=Existing Signer Test Identity' -CertStoreLocation Cert:\CurrentUser\My `
        -KeyAlgorithm RSA -KeyLength 2048 -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddDays(1)
    $overrideDirectory = Join-Path $tempDirectory 'existing certificate'
    & $scriptPath -Action Initialize -OutputDirectory $overrideDirectory -Publisher $Publisher `
        -CertificateThumbprint $overrideCertificate.Thumbprint
    $overrideBinary = Join-Path $tempDirectory 'existing certificate binary.exe'
    Copy-Item -LiteralPath $BinaryPath -Destination $overrideBinary
    & $scriptPath -Action Sign -FilePath $overrideBinary -Publisher $Publisher `
        -CertificateThumbprint $overrideCertificate.Thumbprint
    Assert-VerificationResult -Path $overrideBinary -Thumbprint $overrideCertificate.Thumbprint -ShouldSucceed

    $controlledBuild = Join-Path $tempDirectory 'certificate controls build'
    $sourceDirectory = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
    & cmake -S $sourceDirectory -B $controlledBuild `
        "-DTB_WINDOWS_SIGNING_THUMBPRINT=$($overrideCertificate.Thumbprint)" `
        -DTB_BUILD_TESTS=OFF -DTB_BUILD_BURROW=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Existing-certificate configure failed.' }
    & cmake --build $controlledBuild --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Existing-certificate build failed.' }
    $controlledLibrary = Join-Path $controlledBuild 'bin/badger.dll'
    Assert-VerificationResult -Path $controlledLibrary -Thumbprint $overrideCertificate.Thumbprint -ShouldSucceed

    & cmake -S $sourceDirectory -B $controlledBuild `
        "-DTB_WINDOWS_SIGNING_THUMBPRINT=$expectedThumbprint"
    if ($LASTEXITCODE -ne 0) { throw 'Replacement-certificate configure failed.' }
    & cmake --build $controlledBuild --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Incremental replacement-certificate build failed.' }
    Assert-VerificationResult -Path $controlledLibrary -Thumbprint $expectedThumbprint -ShouldSucceed

    & cmake -S $sourceDirectory -B $controlledBuild -DTB_WINDOWS_SELF_SIGN=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Opt-out configure failed.' }
    & cmake --build $controlledBuild --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Opt-out build failed.' }
    if ((Get-AuthenticodeSignature -LiteralPath $controlledLibrary).Status -ne 'NotSigned') {
        throw 'The opt-out must produce an unsigned engine DLL.'
    }
    Write-Host 'Verified local identity reuse, existing-certificate override, opt-out, and rejection of invalid signatures.'
}
finally {
    if ($overrideCertificate) {
        Remove-Item -LiteralPath "Cert:\CurrentUser\My\$($overrideCertificate.Thumbprint)" -DeleteKey
    }
    Remove-Item -LiteralPath $tempDirectory -Recurse -Force
}
