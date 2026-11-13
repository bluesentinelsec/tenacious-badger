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
        $certificate = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
            Where-Object { $_.Subject -eq $subject -and $_.FriendlyName -eq $friendlyName } |
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

    $signature = Get-AuthenticodeSignature -LiteralPath $BinaryPath
    if (-not $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $ExpectedThumbprint) {
        throw "Missing signature or unexpected signer: $BinaryPath"
    }
    # Trust is deliberately local/ad-hoc. HashMismatch and every other error
    # remain fatal; an untrusted chain is the only permitted verification error.
    if ($signature.Status -notin @('Valid', 'NotTrusted')) {
        throw "Signature verification failed ($($signature.Status)): $BinaryPath -- $($signature.StatusMessage)"
    }
    Write-Host "Verified Authenticode: $BinaryPath [$($signature.Status)]"
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
