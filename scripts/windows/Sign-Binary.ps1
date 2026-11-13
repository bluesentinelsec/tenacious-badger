#Requires -Version 5.1
<#
.SYNOPSIS
Creates/reuses a local self-signing identity and signs or verifies Windows PE files.
.DESCRIPTION
Generated private keys stay non-exportable in CurrentUser\My. No trust store is changed.
Verification pins the signer and accepts an untrusted chain, but rejects altered
content, missing signatures, and other verification errors. No network is needed.
#>
[CmdletBinding()]
param(
    [ValidateSet('Initialize', 'Sign', 'Verify', 'VerifyManifest')]
    [string]$Action = 'Sign',
    [string]$FilePath = '',
    [string]$OutputDirectory = '',
    [string]$CertificateThumbprint = '',
    [string]$CertificateFile = '',
    [ValidatePattern('^[A-Za-z0-9_.-]+$')]
    [string]$Publisher = 'bluesentinelsec'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Windows Authenticode signing requires Windows.'
}

function Get-SigningCertificate {
    param([string]$Thumbprint, [string]$PublisherName)

    $subject = "CN=$PublisherName"
    $friendlyName = "Tenacious Badger self-signing ($PublisherName)"
    if ($Thumbprint) {
        if ($Thumbprint -notmatch '^[A-Fa-f0-9]{40}$') {
            throw 'CertificateThumbprint must be a 40-character certificate-store thumbprint.'
        }
        $certificate = Get-Item -LiteralPath "Cert:\CurrentUser\My\$Thumbprint"
    }
    else {
        $certificate = Get-ChildItem -LiteralPath Cert:\CurrentUser\My |
            Where-Object { $_.HasPrivateKey -and $_.Subject -eq $subject -and $_.FriendlyName -eq $friendlyName } |
            Sort-Object NotAfter -Descending | Select-Object -First 1
        if (-not $certificate) {
            $certificate = New-SelfSignedCertificate -Type CodeSigningCert `
                -Subject $subject -FriendlyName $friendlyName `
                -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA `
                -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
                -NotAfter (Get-Date).AddYears(3)
        }
    }
    if (-not $certificate.HasPrivateKey) {
        throw 'Select a certificate with its private key in CurrentUser\My.'
    }
    if ($certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date)) {
        throw 'The signing certificate is outside its validity period. Select a replacement with TB_WINDOWS_SIGNING_THUMBPRINT.'
    }
    $codeSigning = @($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq '1.3.6.1.5.5.7.3.3' })
    if ($codeSigning.Count -eq 0) {
        throw 'The selected certificate must have the Code Signing extended key usage.'
    }
    return $certificate
}

function Assert-BinarySignature {
    param([string]$BinaryPath, [string]$ExpectedThumbprint)

    if (-not ('TenaciousBadger.Signing.NativeVerification' -as [type])) {
        # Windows reports a self-signed root as a generic PowerShell signature
        # error. Inspect the native status so no other failure is accepted.
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace TenaciousBadger.Signing {
    public static class NativeVerification {
        public const uint UntrustedRoot = 0x800B0109u;
        [StructLayout(LayoutKind.Sequential)]
        private struct FileInfo {
            public uint Size;
            public IntPtr Path;
            public IntPtr Handle;
            public IntPtr KnownSubject;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct TrustData {
            public uint Size;
            public IntPtr PolicyCallback;
            public IntPtr SipClient;
            public uint UiChoice;
            public uint RevocationChecks;
            public uint UnionChoice;
            public IntPtr File;
            public uint StateAction;
            public IntPtr State;
            public IntPtr Url;
            public uint Flags;
            public uint UiContext;
            public IntPtr SignatureSettings;
        }
        [DllImport("wintrust.dll", ExactSpelling = true)]
        private static extern uint WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
        public static uint Verify(string path) {
            IntPtr nativePath = IntPtr.Zero;
            IntPtr nativeFile = IntPtr.Zero;
            TrustData data = new TrustData();
            Guid action = new Guid("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");
            try {
                nativePath = Marshal.StringToCoTaskMemUni(path);
                FileInfo file = new FileInfo();
                file.Size = (uint)Marshal.SizeOf(typeof(FileInfo));
                file.Path = nativePath;
                nativeFile = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
                Marshal.StructureToPtr(file, nativeFile, false);
                data.Size = (uint)Marshal.SizeOf(typeof(TrustData));
                data.UiChoice = 2; // WTD_UI_NONE
                data.UnionChoice = 1; // WTD_CHOICE_FILE
                data.File = nativeFile;
                data.StateAction = 1; // WTD_STATEACTION_VERIFY
                data.Flags = 0x1010; // CACHE_ONLY_URL_RETRIEVAL | REVOCATION_CHECK_NONE
                return WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            }
            finally {
                if (data.State != IntPtr.Zero) {
                    data.StateAction = 2; // WTD_STATEACTION_CLOSE
                    WinVerifyTrust(new IntPtr(-1), ref action, ref data);
                }
                if (nativeFile != IntPtr.Zero) { Marshal.FreeHGlobal(nativeFile); }
                if (nativePath != IntPtr.Zero) { Marshal.FreeCoTaskMem(nativePath); }
            }
        }
    }
}
'@
    }
    $resolvedPath = (Resolve-Path -LiteralPath $BinaryPath).Path
    $rawSigner = [Security.Cryptography.X509Certificates.X509Certificate]::CreateFromSignedFile($resolvedPath)
    $signer = [Security.Cryptography.X509Certificates.X509Certificate2]::new($rawSigner)
    try {
        if ($signer.Thumbprint -ne $ExpectedThumbprint) {
            throw "Unexpected signer: $BinaryPath"
        }
        $result = [TenaciousBadger.Signing.NativeVerification]::Verify($resolvedPath)
        if ($result -ne 0 -and $result -ne [TenaciousBadger.Signing.NativeVerification]::UntrustedRoot) {
            throw ("Authenticode verification failed (0x{0:X8}): {1}" -f $result, $BinaryPath)
        }
        Write-Host ("Verified embedded Authenticode: {0} [0x{1:X8}]" -f $BinaryPath, $result)
    }
    finally {
        $signer.Dispose()
        $rawSigner.Dispose()
    }
}

if ($Action -in @('Initialize', 'Sign')) {
    # Serialize certificate creation across concurrent MSBuild/CMake processes.
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $mutex = [Threading.Mutex]::new($false, "Local\TenaciousBadger-SelfSigning-$sid")
    $acquired = $false
    try {
        try {
            $acquired = $mutex.WaitOne(60000)
        }
        catch [Threading.AbandonedMutexException] {
            $acquired = $true
        }
        if (-not $acquired) {
            throw 'Timed out waiting for the local signing identity.'
        }
        $certificate = Get-SigningCertificate -Thumbprint $CertificateThumbprint -PublisherName $Publisher
    }
    finally {
        if ($acquired) {
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
    }
    $CertificateThumbprint = $certificate.Thumbprint
}
elseif ($CertificateFile) {
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new(
        (Resolve-Path -LiteralPath $CertificateFile).Path)
    $CertificateThumbprint = $certificate.Thumbprint
}
if ($CertificateThumbprint -notmatch '^[A-Fa-f0-9]{40}$') {
    throw 'Verification requires CertificateThumbprint or a public CertificateFile.'
}

switch ($Action) {
    'Initialize' {
        if (-not $OutputDirectory) {
            throw 'Initialize requires OutputDirectory.'
        }
        $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
        $publicPath = Join-Path $OutputDirectory 'self-signing.cer'
        $null = Export-Certificate -Cert $certificate -FilePath $publicPath -Force
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'thumbprint.txt') `
            -Value $CertificateThumbprint -Encoding Ascii
        $signingType = 'Authenticode'
        if ($certificate.Subject -eq $certificate.Issuer) {
            $signingType = 'self-signed Authenticode'
        }
        $metadata = [ordered]@{
            type = $signingType
            publisher = $Publisher
            subject = $certificate.Subject
            thumbprint = $CertificateThumbprint
            certificate_sha256 = (Get-FileHash -LiteralPath $publicPath -Algorithm SHA256).Hash
            digest = 'SHA256'
            not_after = $certificate.NotAfter.ToUniversalTime().ToString('o')
            public_certificate = 'self-signing.cer'
        }
        $metadata | ConvertTo-Json | Set-Content `
            -LiteralPath (Join-Path $OutputDirectory 'signing.json') -Encoding Ascii
        Write-Host "Using local self-signing identity: $($certificate.Subject) [$CertificateThumbprint]"
    }
    'Sign' {
        if (-not $FilePath -or -not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
            throw "Sign requires an existing binary: $FilePath"
        }
        if ([IO.Path]::GetExtension($FilePath).ToLowerInvariant() -notin
            @('.exe', '.dll', '.pyd', '.ocx', '.cpl', '.scr', '.sys', '.efi')) {
            throw 'Only Windows PE binaries are signed; static libraries, headers, and symbols are not PE images.'
        }
        $null = Set-AuthenticodeSignature -LiteralPath $FilePath `
            -Certificate $certificate -HashAlgorithm SHA256 -IncludeChain Signer
        Assert-BinarySignature -BinaryPath $FilePath -ExpectedThumbprint $CertificateThumbprint
    }
    'Verify' {
        if (-not $FilePath) {
            throw 'Verify requires FilePath.'
        }
        Assert-BinarySignature -BinaryPath $FilePath -ExpectedThumbprint $CertificateThumbprint
    }
    'VerifyManifest' {
        if (-not $FilePath) {
            throw 'VerifyManifest requires FilePath pointing to a binary-path manifest.'
        }
        $paths = @(Get-Content -LiteralPath $FilePath | Where-Object { $_.Trim() })
        if ($paths.Count -eq 0) {
            throw 'The signing manifest must list at least one binary.'
        }
        foreach ($binary in $paths) {
            Assert-BinarySignature -BinaryPath $binary -ExpectedThumbprint $CertificateThumbprint
        }
    }
}
