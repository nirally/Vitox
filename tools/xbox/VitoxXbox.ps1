#requires -Version 7.2
<#
.SYNOPSIS
  Vitox-only Xbox Developer Mode build/deploy/test/log harness (Device Portal REST).

.DESCRIPTION
  Commands:
    selftest   Offline validation of harness logic (no console traffic).
    status     Read-only console status. First run records the console TLS thumbprint.
    trust      Pin the console TLS thumbprint after the user has confirmed it.
    discover   Read-only LocalAppData/LocalState path discovery for the installed package.
    prepare    Create a run, inspect the package, snapshot, optionally deploy, arm the visual gate.
    launch     Launch the run's package (visual runs need -Confirmation confirmed).
    collect    Download LocalState logs, slice the current session, analyse.
    stop       Stop the app through the Device Portal task manager.
    homebrew-list    (M8A) Read-only listing of DevelopmentFiles\Vitox\homebrew + the current vitox-launch.txt.
                     0.1.17: -AppRoot app-staging lists every staged file (size + SHA-256), read-only.
    homebrew-push    (M8A) Upload a local extracted homebrew folder (-Source; eboot.bin + sce_sys\param.sfo) into
                     DevelopmentFiles\Vitox\homebrew\<-Folder or TITLE_ID>. Upload/create only, never delete.
    homebrew-select  (M8A) Write vitox-launch.txt: -App <folder or TITLE_ID> (fails closed on 0 / >1 matches),
                     -Presenter visual|headless, -TimeoutSec. -AppRoot app-staging selects from the app-staging root.
                     -LicensedLoad (M9A.3A, only with -AppRoot app-staging -FailClosed -Presenter headless) writes
                     licensed_load=staged-license: the launcher validates the app + its staged license, then STOPs.
    app-push         (M9A.2C) Upload a local app folder (-Source; eboot.bin + sce_sys\param.sfo) into
                     DevelopmentFiles\Vitox\app-staging\<TITLE_ID> (-TitleId must equal the param.sfo TITLE_ID). Upload/
                     create only, never delete or overwrite; license material in the source is refused; every file is
                     size- and SHA-256-verified by read-back. -DryRun validates and prints the plan with NO console traffic.
    data-backup      (0.1.18, M9A.5) READ-ONLY on the console: download -TitleId's guest save data
                     (vita-fs ux0:user/00/savedata/<TITLE_ID>) and the guest trophy tree (ux0:user/00/trophy) into a NEW
                     timestamped PC folder under DATA-BACKUPS (never overwritten), size- and SHA-256-checked, + manifest.
    license-push     (M9A.2C) Stage one license file (-License) for a staged app (-TitleId) into
                     DevelopmentFiles\Vitox\app-staging\licenses\<CONTENT_ID>\work.bin, apart from every app. The file's
                     size, content ID and derived TITLE_ID must match the app's param.sfo; -DryRun needs -Source (the
                     local app folder) and sends nothing.

  0.1.9: prepare stops a running Vitox instance before deploy/launch by default; -KeepRunningDiagnostic keeps
  it running (explicit diagnostic override only).
  0.1.10 (M8A, user-approved 2026-10-20): the ONLY file writes are scoped uploads/folder creation under
  DevelopmentFiles\Vitox\homebrew (Test-HomebrewScope); every other file write is still refused.

  Safety (enforced in code): HTTPS only; no 'auto-' usernames; only allow-listed
  endpoints; no uninstall, no file delete, no settings writes; no log clearing;
  install/update only for matching Name+Publisher with a strictly higher version.
  -DryRun prints state-changing requests instead of sending them (GETs still run).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('selftest', 'status', 'trust', 'discover', 'prepare', 'launch', 'collect', 'stop', 'homebrew-list', 'homebrew-push', 'homebrew-select', 'firmware-push', 'app-push', 'license-push', 'data-backup', 'screenshot', 'uninstall')]
    [string]$Command,
    [string]$Config = (Join-Path $PSScriptRoot 'vitox-xbox.config.json'),
    [string]$Package,
    [string]$Label = 'run',
    [string]$Run,
    [switch]$Visual,
    [switch]$Deploy,
    [string]$Confirmation,
    [string]$Thumbprint,
    [switch]$ReplacePin,
    [switch]$AllowStaleUserRegistration,
    [switch]$KeepRunningDiagnostic,
    [int]$MonitorSec = 20,
    [switch]$DryRun,
    [string]$Source,
    [string]$Folder,
    [string]$App,
    [ValidateSet('visual', 'headless')]
    [string]$Presenter = 'visual',
    [ValidateRange(5, 3600)] # 0.1.18: up to 60 min (M9A.5 soak)
    [int]$TimeoutSec = 60,
    [switch]$Trap,
    [switch]$FailClosed,
    [switch]$StagedPgf,
    [switch]$SyncPipelines, # 0.1.21 (M9A.6): vitox-launch.txt async_pipelines=off
    [ValidatePattern('^\d{1,3},\d{1,3},\d{1,4},\d{1,4}(,\d{2,5})?(;\d{1,3},\d{1,3},\d{1,4},\d{1,4}(,\d{2,5})?){0,5}$')]
    [string]$AutoTouch, # 0.1.22 (M9A.6 diagnostic): vitox-launch.txt auto_touch=x,y,from_s,to_s[,period_ms][;...] (scripted taps, 0.1.24: up to 6 segments)
    [ValidatePattern('^[0-9.]+,[0-9.]+,-?[0-9.]+,-?[0-9.]+(,[0-9]+)?(;[0-9.]+,[0-9.]+,-?[0-9.]+,-?[0-9.]+(,[0-9]+)?){0,5}$')] # 0.1.27: optional 5th field = held button mask (launcher 0.0.24.98)
    [string]$AutoStick, # 0.1.24 (M9A.6 diagnostic): vitox-launch.txt auto_stick=from,to,lx,ly[;...] (scripted left stick)
    [string]$ImeInput,
    [ValidateSet('launcher', 'direct')]
    [string]$Ui, # 0.1.32 (launcher 0.0.25.0): vitox-launch.txt ui= (visual runs: launcher = the library UI opens, direct = boot -App at once)
    [string]$UiScript, # 0.1.32 (DIAGNOSTIC): scripted pad buttons for the launcher UI / quick menu, uploaded as ui-script.txt
    [string]$Profiles, # 0.1.32: per-title launch profiles for the library UI, uploaded as homebrew\vitox-profiles.txt
    [string]$SgdbKey, # 0.1.33 (launcher 0.0.26.0): a text file with the user's SteamGridDB API key, uploaded as homebrew\steamgriddb-key.txt (the launcher imports it)
    [ValidateSet('homebrew', 'app-staging')]
    [string]$AppRoot,
    [ValidatePattern('^(libc|libfios2|libult|libpvf|libSceFt2|libscemp4|libface|libsmart)(,(libc|libfios2|libult|libpvf|libSceFt2|libscemp4|libface|libsmart)){0,7}$')] # 0.1.20: libpvf/libSceFt2; 0.1.23: libscemp4 (M9A.6, user-approved 2026-10-27); 0.1.26: libface/libsmart (M9A.7, upstream auto-LLE)
    [string]$LleModules,
    [string]$TitleId,
    [string]$License,
    [switch]$LicensedLoad
)

Set-StrictMode -Version 1.0  # console JSON has optional fields; v2+ throws on absent properties
$ErrorActionPreference = 'Stop'
$script:HarnessVersion = '0.1.34' # 0.1.34: process match accepts Vitox.exe (0.1.1.12 file rename) and VitoxM1Probe.exe
$script:DryRun = [bool]$DryRun
$script:StateDir = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Config))) 'state'
$script:RunDir = $null
$script:GatePhrase = 'confirmed'
# Any milestone probe (M1, M2, M2.5, ...) shares the identity and the log layout.
$script:SessionMarker = '^.*===== Vitox M\d+(?:\.\d+)? Probe session start pid=(\d+) =====\s*$'
$script:ExitPinMismatch = 3  # status: console presents a certificate other than the pinned one

if (-not ('VitoxHarness.Tls' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'VitoxXbox.Native.cs')
}

# ---------------------------------------------------------------- utilities

function Write-Json($Object, [string]$Path) {
    $Object | ConvertTo-Json -Depth 12 | Set-Content -Path $Path -Encoding utf8
}

function Read-Json([string]$Path) {
    Get-Content -Raw -Path $Path | ConvertFrom-Json -Depth 12
}

function ConvertTo-Hex64([string]$Text) {
    # Device Portal "hex64" == Base64(UTF-8), per microsoft/WindowsDevicePortalWrapper Utilities.Hex64Encode.
    [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text))
}

function Fail([string]$Message) { throw "VITOX-HARNESS: $Message" }

# ---------------------------------------------------------------- config

function Test-HarnessConfig($Cfg) {
    $errors = @()
    if (-not $Cfg.ConsoleUrl) { $errors += 'ConsoleUrl missing' }
    elseif ($Cfg.ConsoleUrl -notmatch '^https://[^/\s]+(:\d+)?/?$') { $errors += "ConsoleUrl must be https://host[:port] (plain HTTP is forbidden): $($Cfg.ConsoleUrl)" }
    if ($Cfg.AuthEnabled) {
        if (-not $Cfg.Username) { $errors += 'AuthEnabled but Username missing' }
        elseif ($Cfg.Username -like 'auto-*') { $errors += "'auto-' usernames are forbidden (they bypass CSRF protection)" }
        if (-not $Cfg.CredentialFile) { $errors += 'AuthEnabled but CredentialFile missing' }
    }
    if ($Cfg.PinnedCertSha256 -and $Cfg.PinnedCertSha256 -notmatch '^[0-9A-Fa-f]{64}$') { $errors += 'PinnedCertSha256 must be 64 hex chars' }
    foreach ($k in 'Name', 'Publisher', 'PackageFamilyName', 'AppId') {
        if (-not $Cfg.Expected.$k) { $errors += "Expected.$k missing" }
    }
    if (-not $Cfg.LogFiles -or @($Cfg.LogFiles).Count -eq 0) { $errors += 'LogFiles missing' }
    return , $errors
}

function Get-HarnessConfig {
    if (-not (Test-Path $Config)) { Fail "config not found: $Config" }
    $cfg = Read-Json $Config
    $errors = Test-HarnessConfig $cfg
    if ($errors.Count) { Fail ("invalid config:`n  " + ($errors -join "`n  ")) }
    return $cfg
}

# Atomic: the new config is written and parsed in full before it replaces the old one
# (the previous file is kept as <config>.prev), so a failed write never loses the current pin.
function Save-HarnessConfig($Cfg) {
    $path = [IO.Path]::GetFullPath($Config)
    $tmp = "$path.tmp"
    Write-Json $Cfg $tmp
    $null = Read-Json $tmp
    if (Test-Path $path) { [IO.File]::Replace($tmp, $path, "$path.prev") } else { Move-Item $tmp $path }
}

# Credential helper (only used when AuthEnabled). DPAPI-protected, current Windows user only.
function Save-VitoxCredential([string]$Path, [pscredential]$Credential) {
    if ($Credential.UserName -like 'auto-*') { Fail "'auto-' usernames are forbidden" }
    $Credential | Export-Clixml -Path $Path
}
function Get-VitoxCredential([string]$Path) {
    $c = Import-Clixml -Path $Path
    if ($c -isnot [pscredential]) { Fail "credential file is not a PSCredential: $Path" }
    if ($c.UserName -like 'auto-*') { Fail "'auto-' usernames are forbidden" }
    return $c
}

# ---------------------------------------------------------------- HTTP

# Every request must match this list. Anything else (uninstall, file delete, settings PUT, ...) is refused.
$script:Allowed = @(
    'GET /api/os/info', 'GET /api/os/devicefamily', 'GET /api/os/machinename',
    'GET /api/app/packagemanager/packages', 'GET /api/app/packagemanager/state',
    'GET /api/filesystem/apps/knownfolders', 'GET /api/filesystem/apps/files', 'GET /api/filesystem/apps/file',
    'GET /api/resourcemanager/processes', 'GET /api/debug/dump/usermode/dumps', 'GET /api/wer/reports',
    'GET /ext/xbox/info', 'GET /ext/settings',
    'GET /ext/screenshot', # 0.1.28: read-only console screenshot (PNG of the current screen; no state change)
    'POST /api/app/packagemanager/package',
    'POST /api/taskmanager/app', 'DELETE /api/taskmanager/app'
)

# 0.1.29: package uninstall is NOT allow-listed; it passes only for a Vitox.M1Probe full name (Invoke-Uninstall).
function Test-VitoxUninstallScope([string]$Method, [string]$Path, $Query) {
    if ($Method.ToUpperInvariant() -ne 'DELETE' -or $Path -ne '/api/app/packagemanager/package' -or -not $Query -or $Query.Count -ne 1) { return $false }
    return ([string]$Query['package']) -cmatch '^Vitox\.M1Probe_[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+_x64__1baxdpdgcd2t6$'
}

function Test-EndpointAllowed([string]$Method, [string]$Path) {
    return $script:Allowed -contains ("{0} {1}" -f $Method.ToUpperInvariant(), $Path)
}

# 0.1.10 (M8A): the only file writes. Upload (POST file) and folder creation (POST folder) are allowed ONLY with
# knownfolderid=DevelopmentFiles and a path at or below \Vitox\homebrew (plain name segments, no '.'/'..'), plus the
# single bootstrap 'create folder homebrew in \Vitox'. No other query keys (no packagefullname, no extract). No
# delete/rename endpoint exists in either list.
$script:HomebrewRoot = '\Vitox\homebrew'
# 0.1.11 (M8C.1, user-approved 2026-10-20): exactly one more write root, for the staged firmware installer's inputs.
$script:FirmwareRoot = '\Vitox\firmware-staging'
$script:WriteRoots = @($script:HomebrewRoot, $script:FirmwareRoot)
$script:ScopedWrites = @('POST /api/filesystem/apps/file', 'POST /api/filesystem/apps/folder')
function Test-HomebrewName([string]$Name) {
    return ($Name -cmatch '^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$') -and ($Name -notmatch '\.\.')
}
function Test-HomebrewPath([string]$P) {
    foreach ($root in $script:WriteRoots) {
        if ($P -ceq $root) { return $true }
        if (-not $P.StartsWith($root + '\', [StringComparison]::Ordinal)) { continue }
        $ok = $true
        foreach ($seg in $P.Substring($root.Length + 1).Split('\')) { if (-not (Test-HomebrewName $seg)) { $ok = $false } }
        if ($ok) { return $true }
    }
    return $false
}
# 0.1.15 (M9A.2C): the app-staging root, upload/create only, in exactly two shapes (anything else is refused):
#   \Vitox\app-staging\<TITLE_ID>\...           one app per folder; app-push names it after its param.sfo TITLE_ID
#   \Vitox\app-staging\licenses\<CONTENT_ID>    one license file per content ID, apart from every app (license-push)
# Uploads go only into an app folder (or below) or a content-ID folder - never into the root or licenses itself.
$script:AppStagingRoot = '\Vitox\app-staging'
$script:LicenseFolder = 'licenses'
$script:TitleIdPattern = '\A[A-Z0-9]{9}\z'
$script:ContentIdPattern = '\A[A-Z]{2}[0-9]{4}-[A-Z0-9]{9}_[0-9]{2}-[A-Z0-9]{16}\z'
function Test-AppStagingPath([string]$P, [switch]$ForFile) {
    if ($P -ceq $script:AppStagingRoot) { return -not $ForFile }
    if (-not $P.StartsWith($script:AppStagingRoot + '\', [StringComparison]::Ordinal)) { return $false }
    $segs = $P.Substring($script:AppStagingRoot.Length + 1).Split('\')
    if ($segs[0] -ceq $script:LicenseFolder) {
        if ($segs.Count -eq 1) { return -not $ForFile }
        return $segs.Count -eq 2 -and $segs[1] -cmatch $script:ContentIdPattern
    }
    if ($segs[0] -cnotmatch $script:TitleIdPattern) { return $false }
    foreach ($s in $segs) { if (-not (Test-HomebrewName $s)) { return $false } }
    return $true
}
function Test-HomebrewScope([string]$Method, [string]$Path, $Query) {
    if ($script:ScopedWrites -notcontains ("{0} {1}" -f $Method.ToUpperInvariant(), $Path)) { return $false }
    if (-not $Query) { return $false }
    $isFolder = $Path -eq '/api/filesystem/apps/folder'
    $keys = if ($isFolder) { @('knownfolderid', 'path', 'newfoldername') } else { @('knownfolderid', 'path') }
    foreach ($k in $Query.Keys) { if ($keys -cnotcontains $k) { return $false } }
    foreach ($k in $keys) { if (-not $Query.Contains($k)) { return $false } }
    if ([string]$Query['knownfolderid'] -cne 'DevelopmentFiles') { return $false }
    $p = [string]$Query['path']
    if ($isFolder) {
        $n = [string]$Query['newfoldername']
        if ($p -ceq '\Vitox' -and ($n -ceq 'homebrew' -or $n -ceq 'firmware-staging' -or $n -ceq 'app-staging')) { return $true } # bootstrap: create a root itself
        if (Test-AppStagingPath $p) { return (Test-HomebrewName $n) -and (Test-AppStagingPath "$p\$n") }
        return (Test-HomebrewPath $p) -and (Test-HomebrewName $n)
    }
    if (Test-AppStagingPath $p -ForFile) { return $true }
    return Test-HomebrewPath $p
}

$script:Http = $null
$script:Cookies = $null
$script:CsrfToken = $null
$script:BaseUri = $null
$script:AuthHeader = $null

function Connect-Console($Cfg, [int]$TimeoutSec = 30) {
    $script:Cookies = [System.Net.CookieContainer]::new()
    $script:BaseUri = [uri]($Cfg.ConsoleUrl.TrimEnd('/'))
    $script:Http = [VitoxHarness.Tls]::CreateClient($Cfg.PinnedCertSha256, $TimeoutSec, $script:Cookies)
    # CSRF-Token survives client re-creation (it is a session cookie bound to the console, not the HttpClient).
    $script:AuthHeader = $null
    if ($Cfg.AuthEnabled) {
        $c = Get-VitoxCredential $Cfg.CredentialFile
        $pair = '{0}:{1}' -f $c.UserName, $c.GetNetworkCredential().Password
        $script:AuthHeader = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))
    }
}

function Write-HttpLog($Entry) {
    if ($script:RunDir) {
        ($Entry | ConvertTo-Json -Compress -Depth 6) | Add-Content -Path (Join-Path $script:RunDir 'http.jsonl') -Encoding utf8
    }
}

function Invoke-Wdp {
    param(
        [Parameter(Mandatory)][string]$Method,
        [Parameter(Mandatory)][string]$Path,
        [System.Collections.Specialized.OrderedDictionary]$Query,
        [System.Net.Http.HttpContent]$Content,
        [string]$OutFile,
        [switch]$AllowFailure
    )
    if (-not (Test-EndpointAllowed $Method $Path) -and -not (Test-HomebrewScope $Method $Path $Query) -and -not (Test-VitoxUninstallScope $Method $Path $Query)) { Fail "endpoint not allow-listed: $Method $Path" }
    $qs = ''
    if ($Query -and $Query.Count) {
        $qs = '?' + (($Query.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, [uri]::EscapeDataString([string]$_.Value) }) -join '&')
    }
    $uri = [uri]::new($script:BaseUri, $Path + $qs)
    $isWrite = $Method -ne 'GET'
    if ($isWrite -and $script:DryRun) {
        Write-Host "[DRYRUN] $Method $uri" -ForegroundColor Yellow
        Write-HttpLog @{ t = (Get-Date).ToString('o'); method = $Method; uri = "$uri"; dryrun = $true }
        return [pscustomobject]@{ DryRun = $true; Status = 0; Body = $null; Json = $null; Uri = "$uri" }
    }
    if ($isWrite) {
        # CSRF: prime the CSRF-Token cookie with a GET, then echo it in X-CSRF-Token.
        if (-not $script:CsrfToken) {
            $null = Invoke-Wdp -Method GET -Path '/api/os/info' -AllowFailure
        }
        if (-not $script:CsrfToken) { Fail 'console did not issue a CSRF-Token cookie; refusing state-changing request' }
    }
    $req = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::new($Method), $uri)
    if ($script:AuthHeader) { $null = $req.Headers.TryAddWithoutValidation('Authorization', $script:AuthHeader) }
    if ($isWrite) {
        $null = $req.Headers.TryAddWithoutValidation('Cookie', "CSRF-Token=$($script:CsrfToken)")
        $null = $req.Headers.TryAddWithoutValidation('X-CSRF-Token', $script:CsrfToken)
    }
    if ($Content) { $req.Content = $Content }
    $t0 = Get-Date
    try {
        $resp = $script:Http.SendAsync($req).GetAwaiter().GetResult()
    } catch {
        Write-HttpLog @{ t = $t0.ToString('o'); method = $Method; uri = "$uri"; error = $_.Exception.GetBaseException().Message; tls_seen = [VitoxHarness.Tls]::LastSeenSha256 }
        throw
    }
    $status = [int]$resp.StatusCode
    $sc = $null
    if ($resp.Headers.TryGetValues('Set-Cookie', [ref]$sc)) {
        foreach ($c in $sc) { if ($c -match '(?:^|;\s*)CSRF-Token=([^;]+)') { $script:CsrfToken = $Matches[1] } }
    }
    $body = $null; $json = $null
    if ($OutFile -and $resp.IsSuccessStatusCode) {
        $bytes = $resp.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        [IO.File]::WriteAllBytes($OutFile, $bytes)
    } else {
        $body = $resp.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        if ($body -and $body.TrimStart().StartsWith('{') -or ($body -and $body.TrimStart().StartsWith('['))) {
            try { $json = $body | ConvertFrom-Json -Depth 12 } catch { $json = $null }
        }
    }
    Write-HttpLog @{ t = $t0.ToString('o'); method = $Method; uri = "$uri"; status = $status; ms = [int]((Get-Date) - $t0).TotalMilliseconds }
    if (-not $resp.IsSuccessStatusCode -and -not $AllowFailure) {
        Fail ("$Method $Path -> HTTP $status " + ($(if ($body) { $body.Substring(0, [Math]::Min(400, $body.Length)) } else { '' })))
    }
    return [pscustomobject]@{ DryRun = $false; Status = $status; Body = $body; Json = $json; Uri = "$uri" }
}

# ---------------------------------------------------------------- package inspection

function Get-PackageInfo([string]$PackagePath) {
    $pkg = (Resolve-Path $PackagePath).Path
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($pkg)
    try {
        $entry = $zip.Entries | Where-Object FullName -eq 'AppxManifest.xml'
        if (-not $entry) { Fail "AppxManifest.xml not found in $pkg" }
        $sr = [IO.StreamReader]::new($entry.Open()); $xmlText = $sr.ReadToEnd(); $sr.Close()
        [xml]$xml = $xmlText
        $ns = [Xml.XmlNamespaceManager]::new($xml.NameTable)
        $ns.AddNamespace('f', 'http://schemas.microsoft.com/appx/manifest/foundation/windows10')
        $id = $xml.SelectSingleNode('/f:Package/f:Identity', $ns)
        $apps = @($xml.SelectNodes('/f:Package/f:Applications/f:Application', $ns))
        $deps = @($xml.SelectNodes('/f:Package/f:Dependencies/f:PackageDependency', $ns) | ForEach-Object {
                [pscustomobject]@{ Name = $_.Name; MinVersion = $_.MinVersion; Publisher = $_.Publisher } })
        $caps = @($xml.SelectNodes('/f:Package/f:Capabilities/*', $ns) | ForEach-Object { $_.GetAttribute('Name') })
        $exeName = if ($apps.Count) { $apps[0].Executable } else { $null }
        $rsds = $null
        if ($exeName) {
            $exeEntry = $zip.Entries | Where-Object FullName -eq ($exeName -replace '\\', '/')
            if ($exeEntry) {
                $ms = [IO.MemoryStream]::new(); $s = $exeEntry.Open(); $s.CopyTo($ms); $s.Close()
                $exeBytes = $ms.ToArray()
                $r = [VitoxHarness.DebugId]::ReadPeRsds($exeBytes)
                $rsds = [pscustomobject]@{ Guid = $r[0]; Age = [int]$r[1]; PdbPath = $r[2]; ExeSha256 = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($exeBytes)).Replace('-', '') }
            }
        }
    } finally { $zip.Dispose() }
    if (-not $id) { Fail "Identity element missing in $pkg" }
    $publisherId = [VitoxHarness.PackageId]::PublisherId($id.Publisher)
    $arch = if ($id.ProcessorArchitecture) { $id.ProcessorArchitecture.ToLowerInvariant() } else { 'neutral' }
    $fi = Get-Item $pkg
    [pscustomobject]@{
        Path = $pkg; FileName = $fi.Name; Size = $fi.Length
        Sha256 = (Get-FileHash $pkg -Algorithm SHA256).Hash
        Name = $id.Name; Publisher = $id.Publisher; Version = $id.Version; Architecture = $arch
        PublisherId = $publisherId
        PackageFamilyName = "$($id.Name)_$publisherId"
        PackageFullName = "$($id.Name)_$($id.Version)_$($arch)__$publisherId"
        AppIds = @($apps | ForEach-Object { $_.Id }); Executable = $exeName
        Capabilities = $caps; Dependencies = $deps; ExeDebugId = $rsds
    }
}

function Test-Symbols($Info) {
    # Expected layout: releases\<ver>\<pkg>.msix + releases\<ver>\symbols\*.pdb
    $symDir = Join-Path (Split-Path $Info.Path) 'symbols'
    $result = [ordered]@{ SymbolsDir = $symDir; Pdb = $null; PdbGuid = $null; PdbAge = $null; Match = $false; Reason = $null }
    if (-not $Info.ExeDebugId) { $result.Reason = 'package exe has no RSDS debug id'; return [pscustomobject]$result }
    $pdbName = Split-Path $Info.ExeDebugId.PdbPath -Leaf
    $pdb = Join-Path $symDir $pdbName
    if (-not (Test-Path $pdb)) { $result.Reason = "symbols missing: $pdb"; return [pscustomobject]$result }
    $p = [VitoxHarness.DebugId]::ReadPdbInfo($pdb)
    $result.Pdb = $pdb; $result.PdbGuid = $p[0]; $result.PdbAge = [int]$p[1]
    $result.Match = ($p[0] -eq $Info.ExeDebugId.Guid)
    $result.Reason = if ($result.Match) { 'PDB GUID matches exe RSDS GUID' } else { "PDB GUID $($p[0]) != exe RSDS GUID $($Info.ExeDebugId.Guid)" }
    return [pscustomobject]$result
}

function Test-ReleaseHashes([string]$PackagePath) {
    $dir = Split-Path (Resolve-Path $PackagePath).Path
    $sums = Join-Path $dir 'SHA256SUMS.txt'
    if (-not (Test-Path $sums)) { return [pscustomobject]@{ Present = $false; Ok = $false; Mismatches = @('SHA256SUMS.txt missing') } }
    $bad = @()
    foreach ($line in Get-Content $sums) {
        if ($line -notmatch '^([0-9A-F]{64})\s+(\d+)\s+(.+)$') { continue }
        $f = Join-Path $dir $Matches[3]
        if (-not (Test-Path $f)) { $bad += "missing $($Matches[3])"; continue }
        if ((Get-FileHash $f -Algorithm SHA256).Hash -ne $Matches[1]) { $bad += "hash mismatch $($Matches[3])" }
    }
    [pscustomobject]@{ Present = $true; Ok = ($bad.Count -eq 0); Mismatches = $bad }
}

function Test-ExpectedIdentity($Info, $Cfg) {
    $p = @()
    if ($Info.Name -cne $Cfg.Expected.Name) { $p += "Name '$($Info.Name)' != expected '$($Cfg.Expected.Name)'" }
    if ($Info.Publisher -cne $Cfg.Expected.Publisher) { $p += "Publisher '$($Info.Publisher)' != expected '$($Cfg.Expected.Publisher)'" }
    if ($Info.PackageFamilyName -cne $Cfg.Expected.PackageFamilyName) { $p += "PFN '$($Info.PackageFamilyName)' != expected '$($Cfg.Expected.PackageFamilyName)'" }
    if ($Info.Architecture -ne 'x64') { $p += "Architecture '$($Info.Architecture)' != x64" }
    if ($Info.AppIds -notcontains $Cfg.Expected.AppId) { $p += "AppId '$($Cfg.Expected.AppId)' not in package" }
    return , $p
}

# ---------------------------------------------------------------- installed-package normalization

# Xbox WDP /api/app/packagemanager/packages reports Name = display name, PackageFamilyName = identity Name,
# PackageRelativeId = AUMID (observed on Xbox OS 10.0.26100.9445). Identity is therefore derived from
# PackageFullName (<Name>_<Version>_<Arch>_<ResourceId>_<PublisherId>) plus Publisher.
function ConvertTo-InstalledIdentity($Pkg) {
    $full = [string]$Pkg.PackageFullName
    $parts = $full.Split('_')
    if ($parts.Count -ne 5) { return [pscustomobject]@{ Valid = $false; PackageFullName = $full; Raw = $Pkg } }
    $ver = $parts[1].Split('.')
    [pscustomobject]@{
        Valid = $true; Name = $parts[0]; Architecture = $parts[2]; ResourceId = $parts[3]; PublisherId = $parts[4]
        PackageFamilyName = "$($parts[0])_$($parts[4])"; PackageFullName = $full; Publisher = $Pkg.Publisher
        DisplayName = $Pkg.Name; AppUserModelId = $Pkg.PackageRelativeId
        Version = [pscustomobject]@{ Major = [int]$ver[0]; Minor = [int]$ver[1]; Build = [int]$ver[2]; Revision = [int]$ver[3] }
        Raw = $Pkg
    }
}

function Get-InstalledIdentities($PackagesJson) {
    return , @(@($PackagesJson.InstalledPackages) | ForEach-Object { ConvertTo-InstalledIdentity $_ } | Where-Object Valid)
}

# ---- run-dir hygiene: only Vitox metadata is saved; other packages on the console are counted, never recorded.

function Test-VitoxItem($Item, $Cfg, [string]$Exe) {
    $exeBase = if ($Exe) { [IO.Path]::GetFileNameWithoutExtension($Exe) } else { $null }
    foreach ($p in $Item.PSObject.Properties) {
        $v = [string]$p.Value
        if (-not $v) { continue }
        if ($v.StartsWith("$($Cfg.Expected.Name)_", [StringComparison]::OrdinalIgnoreCase)) { return $true }  # full name / PFN
        if ($v -match [regex]::Escape($Cfg.Expected.PackageFamilyName)) { return $true }
        if ($exeBase -and $v -match "(^|[\\/_ ])$([regex]::Escape($exeBase))(\.exe)?([._ ]|$)") { return $true }  # dump/report named after the exe
    }
    return $false
}

function Select-VitoxDumps($Json, $Cfg, [string]$Exe) {
    $all = @(if ($Json) { $Json.CrashDumps })
    $keep = @($all | Where-Object { $_ -and (Test-VitoxItem $_ $Cfg $Exe) })
    [ordered]@{ filter = "Vitox only ($($Cfg.Expected.PackageFamilyName))"; ignored_other_packages = $all.Count - $keep.Count; CrashDumps = $keep }
}

function Select-VitoxWer($Json, $Cfg, [string]$Exe) {
    $users = @(); $ignored = 0
    foreach ($u in @(if ($Json) { $Json.WerReports })) {
        $all = @($u.Reports | Where-Object { $_ })
        $keep = @($all | Where-Object { Test-VitoxItem $_ $Cfg $Exe })
        $ignored += $all.Count - $keep.Count
        $users += [ordered]@{ User = $u.User; Reports = $keep }
    }
    [ordered]@{ filter = "Vitox only ($($Cfg.Expected.PackageFamilyName))"; ignored_other_packages = $ignored; WerReports = $users }
}

function Select-VitoxPackages($Json, $Cfg) {
    $all = @(if ($Json) { $Json.InstalledPackages })
    $keep = @($all | Where-Object { $_ -and ((Test-VitoxItem $_ $Cfg $null) -or ([string]$_.PackageFullName -like 'Microsoft.VCLibs.140.00_*')) })
    [ordered]@{ filter = "Vitox + VCLibs only"; total_installed = $all.Count; InstalledPackages = $keep }
}

# ---------------------------------------------------------------- update policy

function Get-Ledger {
    $f = Join-Path $script:StateDir 'deploy-ledger.json'
    if (Test-Path $f) { return @(Read-Json $f) } else { return @() }
}
function Add-LedgerEntry($Info, [string]$RunDir) {
    $f = Join-Path $script:StateDir 'deploy-ledger.json'
    $l = @(Get-Ledger) + [pscustomobject]@{ PackageFullName = $Info.PackageFullName; Version = $Info.Version; Sha256 = $Info.Sha256; DeployedAt = (Get-Date).ToString('o'); Run = $RunDir }
    Write-Json $l $f
}

# 0.1.8: prepare with -AllowStaleUserRegistration. The Xbox can keep an older per-user registration listed next to
# the current one (seen 2026-10-19: 0.0.12.3 for UserMgr4 next to 0.0.12.4). Only when EVERY same-name entry has the
# incoming publisher and PFN, and exactly one entry has the highest version, are the strictly lower ones dropped
# (reported as stale) and the decision made against the highest. Anything else is returned unchanged, so
# Get-UpdateDecision still refuses it as ambiguous.
function Reduce-StaleForDecision($Info, $Installed, [bool]$AllowStale) {
    $same = @($Installed | Where-Object { $_.Name -ceq $Info.Name })
    $res = [ordered]@{ Installed = @($Installed); Stale = @(); Reduced = $false }
    if (-not $AllowStale -or $same.Count -le 1) { return [pscustomobject]$res }
    if (@($same | Where-Object { $_.Publisher -cne $Info.Publisher -or $_.PackageFamilyName -cne $Info.PackageFamilyName }).Count) { return [pscustomobject]$res }
    $ver = { param($e) [version]("{0}.{1}.{2}.{3}" -f $e.Version.Major, $e.Version.Minor, $e.Version.Build, $e.Version.Revision) }
    $top = ($same | ForEach-Object { & $ver $_ } | Measure-Object -Maximum).Maximum
    $high = @($same | Where-Object { (& $ver $_) -eq $top })
    if ($high.Count -ne 1) { return [pscustomobject]$res }
    $res.Stale = @($same | Where-Object { $_ -ne $high[0] } | ForEach-Object PackageFullName)
    $res.Installed = @(@($Installed | Where-Object { $_.Name -cne $Info.Name }) + $high[0])
    $res.Reduced = $true
    return [pscustomobject]$res
}

function Get-UpdateDecision($Info, $Installed, $Ledger) {
    # $Installed: array of {Name, Publisher, PackageFamilyName, PackageFullName, Version:{Major,Minor,Build,Revision}}
    $same = @($Installed | Where-Object { $_.Name -ceq $Info.Name })
    if ($same.Count -eq 0) { return [pscustomobject]@{ Action = 'install'; Allowed = $true; Reason = 'not installed' } }
    if ($same.Count -gt 1) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "ambiguous: $($same.Count) installed packages named $($Info.Name)" } }
    $cur = $same[0]
    if ($cur.Publisher -cne $Info.Publisher) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "publisher mismatch: installed '$($cur.Publisher)' vs incoming '$($Info.Publisher)'" } }
    if ($cur.PackageFamilyName -cne $Info.PackageFamilyName) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "PFN mismatch: installed '$($cur.PackageFamilyName)' vs incoming '$($Info.PackageFamilyName)'" } }
    $iv = [version]("{0}.{1}.{2}.{3}" -f $cur.Version.Major, $cur.Version.Minor, $cur.Version.Build, $cur.Version.Revision)
    $nv = [version]$Info.Version
    if ($nv -gt $iv) { return [pscustomobject]@{ Action = 'update'; Allowed = $true; Reason = "upgrade $iv -> $nv" } }
    if ($nv -lt $iv) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "downgrade $iv -> $nv refused (would require uninstall)" } }
    if ($cur.PackageFullName -cne $Info.PackageFullName) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "same version but full name differs: '$($cur.PackageFullName)' vs '$($Info.PackageFullName)'" } }
    $known = @($Ledger | Where-Object { $_.PackageFullName -ceq $Info.PackageFullName } | Select-Object -Last 1)
    if ($known.Count -and $known[0].Sha256 -eq $Info.Sha256) { return [pscustomobject]@{ Action = 'none'; Allowed = $true; Reason = "already installed: same version and same SHA-256 as deployed by this harness" } }
    if ($known.Count) { return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "same version $nv but SHA-256 differs from the deployed package (bump the version)" } }
    return [pscustomobject]@{ Action = 'refuse'; Allowed = $false; Reason = "same version $nv installed by something other than this harness; content unknown (fail closed)" }
}

# ---------------------------------------------------------------- launch target

# Default: exactly one installed entry, and it is the run package. With -AllowStaleUserRegistration,
# other entries are tolerated only when they are the same identity at a LOWER version (a per-user
# registration the console has not moved yet, observed on Xbox 2026-10-17); collect then verifies
# from the app's own log that the session ran the run's version, and marks the run INVALID if not.
function Select-LaunchTarget($Installed, $RunPackage, [bool]$AllowStale) {
    $all = @($Installed)
    $match = @($all | Where-Object { $_.PackageFullName -ceq $RunPackage.PackageFullName })
    $others = @($all | Where-Object { $_.PackageFullName -cne $RunPackage.PackageFullName })
    $res = [ordered]@{ Ok = $false; Reason = $null; Entry = $null; Stale = @() }
    if ($match.Count -ne 1) { $res.Reason = "run package $($RunPackage.PackageFullName) installed $($match.Count) times (need exactly 1)"; return [pscustomobject]$res }
    $res.Entry = $match[0]
    if ($others.Count -eq 0) { $res.Ok = $true; $res.Reason = 'single installed entry'; return [pscustomobject]$res }
    if (-not $AllowStale) { $res.Reason = "$($all.Count) installed entries ($(@($all | ForEach-Object PackageFullName) -join ', ')); -AllowStaleUserRegistration not given"; return [pscustomobject]$res }
    $runVer = [version]$RunPackage.Version
    foreach ($o in $others) {
        $v = [version]('{0}.{1}.{2}.{3}' -f $o.Version.Major, $o.Version.Minor, $o.Version.Build, $o.Version.Revision)
        if ($o.PackageFamilyName -cne $RunPackage.PackageFamilyName -or $o.Publisher -cne $RunPackage.Publisher) { $res.Reason = "other entry $($o.PackageFullName) is a different identity"; return [pscustomobject]$res }
        if ($v -ge $runVer) { $res.Reason = "other entry $($o.PackageFullName) is not older than the run package ($v >= $runVer)"; return [pscustomobject]$res }
    }
    $res.Ok = $true; $res.Stale = @($others | ForEach-Object PackageFullName)
    $res.Reason = "stale lower-version registration(s) tolerated: $($res.Stale -join ', '); session version will be verified"
    return [pscustomobject]$res
}

# ---------------------------------------------------------------- visual gate

function New-Gate([string]$RunDir) {
    $g = [ordered]@{ state = 'AWAITING_CONFIRMATION'; created = (Get-Date).ToString('o'); nonce = [guid]::NewGuid().ToString(); phrase = $script:GatePhrase }
    Write-Json $g (Join-Path $RunDir 'gate.json')
}

function Test-Gate([string]$RunDir, [string]$Reply) {
    $f = Join-Path $RunDir 'gate.json'
    if (-not (Test-Path $f)) { return [pscustomobject]@{ Ok = $false; Reason = 'no gate.json: visual launch was never prepared (fail closed)' } }
    $g = Read-Json $f
    if ($g.state -ne 'AWAITING_CONFIRMATION') { return [pscustomobject]@{ Ok = $false; Reason = "gate state is '$($g.state)', not AWAITING_CONFIRMATION (re-run prepare -Visual)" } }
    if ($null -eq $Reply) { return [pscustomobject]@{ Ok = $false; Reason = 'no confirmation supplied' } }
    if ($Reply.Trim() -ine $script:GatePhrase) { return [pscustomobject]@{ Ok = $false; Reason = "confirmation text is not exactly '$($script:GatePhrase)'" } }
    return [pscustomobject]@{ Ok = $true; Reason = 'confirmed' }
}

function Set-GateConfirmed([string]$RunDir, [string]$Reply) {
    $f = Join-Path $RunDir 'gate.json'
    $g = Read-Json $f
    $g.state = 'CONFIRMED'
    $g | Add-Member -NotePropertyName confirmed_at -NotePropertyValue (Get-Date).ToString('o') -Force
    $g | Add-Member -NotePropertyName reply -NotePropertyValue $Reply.Trim() -Force
    Write-Json $g $f
}

# ---------------------------------------------------------------- runs

function New-RunDir([string]$Root, [string]$RunLabel) {
    $safe = ($RunLabel -replace '[^A-Za-z0-9._-]', '_')
    $dir = Join-Path $Root ("{0}_{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $safe)
    if (Test-Path $dir) { Fail "run dir already exists: $dir" }
    foreach ($d in '', 'pre', 'post', 'logs', 'crash') { $null = New-Item -ItemType Directory -Force (Join-Path $dir $d) }
    return $dir
}

function Get-RunJson([string]$RunDir) { Read-Json (Join-Path $RunDir 'run.json') }
function Save-RunJson([string]$RunDir, $Obj) { Write-Json $Obj (Join-Path $RunDir 'run.json') }

# ---------------------------------------------------------------- logs

function Split-Sessions([string[]]$Lines) {
    $sessions = [System.Collections.Generic.List[object]]::new()
    $cur = $null
    foreach ($l in $Lines) {
        if ($l -match $script:SessionMarker) {
            $cur = [pscustomobject]@{ Pid = [int]$Matches[1]; Lines = [System.Collections.Generic.List[string]]::new() }
            $sessions.Add($cur)
        }
        if ($cur) { $cur.Lines.Add($l) }
    }
    return , $sessions
}

# Sessions appended after the first $PreCount ones (the logs are append-only; PIDs can repeat).
function Select-NewSessions($Sessions, [int]$PreCount) {
    # Pipeline output (no unary comma): callers wrap it in @(), so an empty result counts 0.
    @($Sessions) | Select-Object -Skip $PreCount
}

function Get-LocalStateFiles($Cfg, [string]$FullName) {
    $q = [ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $FullName; path = $Cfg.LocalStatePath }
    $r = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query $q -AllowFailure
    if ($r.Status -ne 200 -or -not $r.Json) { return $null }
    return @($r.Json.Items)
}

function Save-LocalStateLogs($Cfg, [string]$FullName, [string]$Dest) {
    $saved = @()
    $listing = Get-LocalStateFiles $Cfg $FullName
    $names = if ($listing) { @($listing | ForEach-Object Name) } else { @() }
    foreach ($f in $Cfg.LogFiles) {
        if ($listing -and $names -notcontains $f) { continue }
        $q = [ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $FullName; filename = $f; path = $Cfg.LocalStatePath }
        $out = Join-Path $Dest $f
        $r = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/file' -Query $q -OutFile $out -AllowFailure
        if ($r.Status -eq 200) { $saved += $f }
    }
    return , $saved
}

function Get-SessionAnalysis([string]$LogsDir) {
    $a = [ordered]@{}
    $read = { param($n) $p = Join-Path $LogsDir $n; if (Test-Path $p) { @(Get-Content $p) } else { @() } }
    $life = & $read 'lifecycle_log.txt'; $probe = & $read 'probe_log.txt'; $fs = & $read 'fs_log.txt'; $mem = & $read 'mem_log.txt'
    $grab = { param($lines, $re) foreach ($l in $lines) { if ($l -match $re) { return $Matches } } return $null }
    $m = & $grab $life 'device: family=(\S+) os_version=(\S+)'; if ($m) { $a.device_family = $m[1]; $a.os_version = $m[2] }
    $m = & $grab $life 'package: name=\S+ publisher=.* version=(\S+) family'; if ($m) { $a.package_version = $m[1] }
    $m = & $grab $life 'previous_session_state=(.*)$'; if ($m) { $a.previous_session_state = $m[1] }
    $m = & $grab $life 'gfx init (\S+)'; if ($m) { $a.gfx_init = $m[1] }
    $m = & $grab $life 'gfx: D3D11CreateDevice hr=(\S+) feature_level=(\S+)'; if ($m) { $a.d3d11 = "hr=$($m[1]) fl=$($m[2])" }
    $m = & $grab $life 'probes complete: (.*)$'; if ($m) { $a.probes_complete = $m[1] }
    $a.lifecycle_events = @($life | ForEach-Object { if ($_ -match '\]\s(Activated|Suspending|Resuming|EnteredBackground|LeavingBackground|VisibilityChanged visible=\d|WindowClosed|Exiting|Uninitialize|UnhandledErrorDetected|AppMemoryUsageLimitChanging)') { $Matches[1] } })
    $a.jit = @($probe | Where-Object { $_ -match 'JIT: ' } | ForEach-Object { ($_ -split '\] ', 2)[1] })
    $a.api = @($probe | ForEach-Object { if ($_ -match 'API (\S+)\s+RESULT=(\S+)') { "$($Matches[1])=$($Matches[2])" } })
    $m = & $grab $probe 'API probe end: (.*)$'; if ($m) { $a.api_summary = $m[1] }
    $a.fs = @($fs | ForEach-Object { if ($_ -match '\[(\w+)\] RESULT (.*)$') { "$($Matches[1]): $($Matches[2])" } })
    $a.fs_errors = @($fs | Where-Object { $_ -match 'FAILED' } | ForEach-Object { ($_ -split '\] ', 2)[1] } | Select-Object -First 12)
    $ticks = @($mem | Where-Object { $_ -match '\] (startup|tick|suspending|resuming|entered_background|leaving_background|usage_level_\w+) usage=' })
    $a.mem_samples = $ticks.Count
    if ($ticks.Count) {
        if ($ticks[0] -match 'usage=(\d+) limit=(\d+).*level=(\w+)') { $a.mem_first = @{ usage = [long]$Matches[1]; limit = [long]$Matches[2]; level = $Matches[3] } }
        if ($ticks[-1] -match 'usage=(\d+) limit=(\d+).*level=(\w+)') { $a.mem_last = @{ usage = [long]$Matches[1]; limit = [long]$Matches[2]; level = $Matches[3] } }
        $a.mem_peak_usage = ($ticks | ForEach-Object { if ($_ -match 'usage=(\d+)') { [long]$Matches[1] } } | Measure-Object -Maximum).Maximum
        $a.mem_first_ts = ($ticks[0] -split ' \+')[0]; $a.mem_last_ts = ($ticks[-1] -split ' \+')[0]
    }
    $a.mem_limit_changes = @($mem | Where-Object { $_ -match 'AppMemoryUsageLimitChanging' } | ForEach-Object { ($_ -split '\] ', 2)[1] })
    $crash = Join-Path $LogsDir 'crash_log.txt'
    $a.crash_lines = if (Test-Path $crash) { @(Get-Content $crash) } else { @() }
    return [pscustomobject]$a
}

# ---------------------------------------------------------------- commands

function New-CertObservation($Cfg, [string]$Kind) {
    [ordered]@{ observed_at = (Get-Date).ToString('o'); kind = $Kind; url = $Cfg.ConsoleUrl; sha256 = [VitoxHarness.Tls]::LastSeenSha256
        pinned_at_observation = if ($Cfg.PinnedCertSha256) { ([string]$Cfg.PinnedCertSha256).ToUpperInvariant() } else { $null }
        subject = [VitoxHarness.Tls]::LastSubject; issuer = [VitoxHarness.Tls]::LastIssuer
        not_before = [VitoxHarness.Tls]::LastNotBefore; not_after = [VitoxHarness.Tls]::LastNotAfter; policy_errors = [VitoxHarness.Tls]::LastPolicyErrors }
}

function Invoke-Status {
    $cfg = Get-HarnessConfig
    $null = New-Item -ItemType Directory -Force $script:StateDir
    Connect-Console $cfg 15
    if ($cfg.PinnedCertSha256) {
        # A non-pinned certificate fails the TLS handshake, so no HTTP request is ever sent to it.
        # Record what was presented (for an explicit 'trust -ReplacePin'); never trust it here.
        $preflight = $null
        try { $null = Invoke-Wdp -Method GET -Path '/api/os/info' } catch { $preflight = $_ }
        $seen = [VitoxHarness.Tls]::LastSeenSha256
        if ($seen -and $seen -ne ([string]$cfg.PinnedCertSha256).ToUpperInvariant()) {
            $obs = New-CertObservation $cfg 'pin-mismatch'
            Write-Json $obs (Join-Path $script:StateDir 'observed-cert.json')
            Write-Host 'PIN_MISMATCH: the console presented a certificate other than the pinned one. Nothing was sent; the pin is unchanged.'
            $obs.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-22} {1}" -f $_.Key, $_.Value) }
            Write-Host "`nREPLACEMENT_REQUIRED. Only after independent verification of the new fingerprint:`n  VitoxXbox.ps1 trust -Thumbprint $seen -ReplacePin"
            exit $script:ExitPinMismatch
        }
        if ($preflight) { throw $preflight }
    }
    if (-not $cfg.PinnedCertSha256) {
        try { $null = Invoke-Wdp -Method GET -Path '/api/os/info' } catch { }
        $seen = [VitoxHarness.Tls]::LastSeenSha256
        if (-not $seen) { Fail "could not reach $($cfg.ConsoleUrl) or no TLS certificate presented" }
        $obs = New-CertObservation $cfg 'unpinned'
        Write-Json $obs (Join-Path $script:StateDir 'observed-cert.json')
        Write-Host 'TLS certificate presented by the console (NOT trusted yet - no data was exchanged):'
        $obs.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-14} {1}" -f $_.Key, $_.Value) }
        Write-Host "`nIf this matches the console, pin it with:`n  VitoxXbox.ps1 trust -Thumbprint $seen"
        exit 2
    }
    $out = [ordered]@{ collected_at = (Get-Date).ToString('o'); console = $cfg.ConsoleUrl; tls_sha256 = [VitoxHarness.Tls]::Pinned }
    foreach ($ep in '/api/os/info', '/api/os/devicefamily', '/api/os/machinename', '/ext/xbox/info') {
        $r = Invoke-Wdp -Method GET -Path $ep -AllowFailure
        $out[$ep] = if ($r.Json) { $r.Json } else { "HTTP $($r.Status)" }
    }
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    $all = Get-InstalledIdentities $pk.Json
    $out.installed_package_count = $all.Count
    $out.vitox = @($all | Where-Object { $_.Name -ceq $cfg.Expected.Name } | Select-Object -Property * -ExcludeProperty Raw)
    $out.vclibs = @($all | Where-Object { $_.Name -eq 'Microsoft.VCLibs.140.00' } | Select-Object Name, PackageFullName, Version)
    $st = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/state' -AllowFailure
    $out.install_state = @{ status = $st.Status; body = $st.Json }
    $set = Invoke-Wdp -Method GET -Path '/ext/settings' -AllowFailure
    $out.settings_status = $set.Status
    if ($set.Json) {
        $out.settings = $set.Json
        $out.settings_game_related = @(@($set.Json.Settings) | Where-Object { $_.Name -match 'game|uwp|apptype|title' } | Select-Object Name, Value, Type, Disabled)
    }
    $pr = Invoke-Wdp -Method GET -Path '/api/resourcemanager/processes' -AllowFailure
    $out.processes_endpoint = $pr.Status
    if ($pr.Json) {
        $out.vitox_process = @(@($pr.Json.Processes) | Where-Object { [string]$_.ImageName -imatch '^Vitox(M1Probe)?(\.exe)?$' })
    }
    $f = Join-Path $script:StateDir 'last-status.json'
    Write-Json $out $f
    Write-Host "Console     : $($cfg.ConsoleUrl) (TLS pinned $([VitoxHarness.Tls]::Pinned.Substring(0,16))...)"
    $osi = $out['/api/os/info']; if ($osi -is [psobject]) { Write-Host "OS          : $($osi.OsEdition) $($osi.OsVersion) ($($osi.ComputerName))" }
    $fam = $out['/api/os/devicefamily']; if ($fam -is [psobject]) { Write-Host "Family      : $($fam.DeviceType)" }
    Write-Host "Packages    : $($all.Count) installed"
    if ($out.vitox.Count) { $out.vitox | ForEach-Object { Write-Host ("Vitox       : {0} (v{1}.{2}.{3}.{4})" -f $_.PackageFullName, $_.Version.Major, $_.Version.Minor, $_.Version.Build, $_.Version.Revision) } }
    else { Write-Host "Vitox       : not installed" }
    Write-Host "VCLibs x64  : $(if ($out.vclibs.Count) { @($out.vclibs | Where-Object PackageFullName -match '_x64_' | ForEach-Object PackageFullName) -join ', ' } else { 'not listed (framework packages are not enumerated by this endpoint on Xbox)' })"
    Write-Host "Install st. : HTTP $($st.Status)"
    Write-Host "Settings    : HTTP $($set.Status); game-related: $(@($out.settings_game_related | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join '; ')"
    Write-Host "Processes   : HTTP $($pr.Status)$(if ($pr.Json) { '; vitox running: ' + [bool]$out.vitox_process.Count })"
    Write-Host "Saved       : $f"
    exit 0  # do not leak an earlier $LASTEXITCODE (e.g. 3 from a previous pin-mismatch status)
}

function Invoke-Trust {
    $cfg = Get-HarnessConfig
    $obsFile = Join-Path $script:StateDir 'observed-cert.json'
    if (-not (Test-Path $obsFile)) { Fail 'no observed certificate; run status first' }
    $obs = Read-Json $obsFile
    if (-not $Thumbprint) { Fail '-Thumbprint is required (the value the user confirmed)' }
    $t = $Thumbprint.Replace(':', '').Replace(' ', '').ToUpperInvariant()
    if ($t -notmatch '^[0-9A-F]{64}$') { Fail "thumbprint must be 64 hex chars (SHA-256): $t" }
    if ($t -ne ([string]$obs.sha256).ToUpperInvariant()) { Fail "thumbprint $t does not match the last observed certificate $($obs.sha256)" }
    $current = if ($cfg.PinnedCertSha256) { ([string]$cfg.PinnedCertSha256).ToUpperInvariant() } else { $null }
    if ($current) {
        if (-not $ReplacePin) { Fail "a thumbprint is already pinned ($current); replacing it requires -ReplacePin" }
        if ($current -eq $t) { Fail "thumbprint $t is already the pinned one; nothing to replace" }
        # Only a mismatch that 'status' observed while the *current* pin was in force can replace it.
        $obsKind = if ($obs.PSObject.Properties['kind']) { $obs.kind } else { $null }
        $obsPin = if ($obs.PSObject.Properties['pinned_at_observation'] -and $obs.pinned_at_observation) { ([string]$obs.pinned_at_observation).ToUpperInvariant() } else { $null }
        if ($obsKind -ne 'pin-mismatch' -or $obsPin -ne $current) {
            Fail "the last observation is not a pin mismatch recorded against the current pin $current (kind=$obsKind, pinned_at_observation=$obsPin); run status first"
        }
        $cfg | Add-Member -NotePropertyName PreviousPinnedCertSha256 -NotePropertyValue $current -Force
        $cfg | Add-Member -NotePropertyName PinReplacedAt -NotePropertyValue (Get-Date).ToString('o') -Force
    }
    $cfg.PinnedCertSha256 = $t
    $cfg | Add-Member -NotePropertyName PinnedAt -NotePropertyValue (Get-Date).ToString('o') -Force
    Save-HarnessConfig $cfg
    $check = (Read-Json $Config).PinnedCertSha256
    if ($check -ne $t) { Fail "pin write verification failed (config holds $check)" }
    if ($current) { Write-Host "Replaced pinned console TLS certificate $current -> $t (previous config kept as $([IO.Path]::GetFullPath($Config)).prev)" }
    else { Write-Host "Pinned console TLS certificate SHA-256 $t" }
}

function Get-InstalledVitox($Cfg) {
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    return , @((Get-InstalledIdentities $pk.Json) | Where-Object { $_.Name -ceq $Cfg.Expected.Name })
}

# 0.1.9: an un-stopped previous Vitox instance correlated with the intermittent M6C host-queue stall, so prepare
# stops any running instance by default. Only the Vitox image counts as "running"; the stop targets only installed
# Vitox package full names (never another package or process). -KeepRunningDiagnostic keeps it (controlled repro).
function Get-RunningVitoxPlan($ProcessesJson, $InstalledVitox, $Cfg, [bool]$Keep) {
    $procs = @(@($ProcessesJson.Processes) | Where-Object { [string]$_.ImageName -imatch '^Vitox(M1Probe)?(\.exe)?$' })
    $targets = @(@($InstalledVitox) | Where-Object { $_.Name -ceq $Cfg.Expected.Name -and $_.Publisher -ceq $Cfg.Expected.Publisher } |
            ForEach-Object { [string]$_.PackageFullName })
    $action = if (-not $procs.Count) { 'none' } elseif ($Keep) { 'keep' } else { 'stop' }
    [pscustomobject]@{
        Found = $procs.Count; Pids = @($procs | ForEach-Object { $_.ProcessId }); Action = $action
        Targets = [string[]]@(if ($action -eq 'stop') { $targets })  # typed: a single target must not unroll to a string
    }
}

function Stop-RunningVitox($Cfg, [bool]$Keep) {
    $pr = Invoke-Wdp -Method GET -Path '/api/resourcemanager/processes'
    $plan = Get-RunningVitoxPlan $pr.Json (Get-InstalledVitox $Cfg) $Cfg $Keep
    $out = [ordered]@{ found = $plan.Found; pids = $plan.Pids; action = $plan.Action; targets = $plan.Targets; stopped = $false; still_running = $null }
    switch ($plan.Action) {
        'none' { Write-Host 'Running     : no Vitox process running' }
        'keep' { Write-Host "Running     : Vitox process(es) $($plan.Pids -join ',') LEFT RUNNING (-KeepRunningDiagnostic: controlled diagnostic only)" }
        'stop' {
            if (-not $plan.Targets.Count) { Fail "Vitox process(es) $($plan.Pids -join ',') running but no installed Vitox package to stop" }
            foreach ($t in $plan.Targets) {
                $r = Invoke-Wdp -Method DELETE -Path '/api/taskmanager/app' -Query ([ordered]@{ package = (ConvertTo-Hex64 $t) }) -AllowFailure
                Write-Host "Running     : Vitox process(es) $($plan.Pids -join ',') found; stop $t -> $(if ($r.DryRun) { 'dry run' } else { "HTTP $($r.Status)" })"
            }
            if (-not $script:DryRun) {
                $left = $plan.Found
                for ($i = 0; $i -lt 10 -and $left; $i++) {
                    Start-Sleep -Seconds 2
                    $p2 = Invoke-Wdp -Method GET -Path '/api/resourcemanager/processes'
                    $left = (Get-RunningVitoxPlan $p2.Json @() $Cfg $false).Found
                }
                $out.still_running = $left
                $out.stopped = -not $left
                if ($left) { Fail "Vitox process still running after stop ($left); prepare refused" }
                Write-Host 'Running     : stopped; no Vitox process running now (LocalState untouched)'
            }
        }
    }
    return $out
}

# Discover the LocalState sub-path from the live listing (never guessed). Returns $null until the app has
# created LocalState (the package data folder is empty before first launch).
function Find-LocalStatePath($Cfg, [string]$FullName) {
    $root = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query ([ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $FullName }) -AllowFailure
    $items = if ($root.Json) { @($root.Json.Items) } else { @() }
    $ls = $items | Where-Object Name -eq 'LocalState' | Select-Object -First 1
    if (-not $ls) { return $null }
    $probe = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query ([ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $FullName; path = $ls.SubPath }) -AllowFailure
    if ($probe.Status -ne 200) { return $null }
    return [string]$ls.SubPath
}

function Save-LocalStatePath($Cfg, [string]$Path) {
    $Cfg | Add-Member -NotePropertyName LocalStatePath -NotePropertyValue $Path -Force
    $Cfg | Add-Member -NotePropertyName LocalStatePathDiscoveredAt -NotePropertyValue (Get-Date).ToString('o') -Force
    Save-HarnessConfig $Cfg
}

function Invoke-Discover {
    $cfg = Get-HarnessConfig
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg
    $inst = Get-InstalledVitox $cfg
    if ($inst.Count -ne 1) { Fail "need exactly one installed $($cfg.Expected.Name) to discover paths (found $($inst.Count))" }
    $full = $inst[0].PackageFullName
    $kf = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/knownfolders' -AllowFailure
    Write-Host "Known folders: HTTP $($kf.Status) $($kf.Body)"
    $root = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query ([ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $full }) -AllowFailure
    Write-Host "Package root listing: HTTP $($root.Status)"
    $items = if ($root.Json) { @($root.Json.Items) } else { @() }
    $items | ForEach-Object { Write-Host ("  {0,-24} type={1} subpath={2}" -f $_.Name, $_.Type, $_.SubPath) }
    $ls = $items | Where-Object Name -eq 'LocalState' | Select-Object -First 1
    if (-not $ls) { Fail 'LocalState entry not found in the package LocalAppData listing' }
    $candidate = $ls.SubPath
    $probe = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query ([ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $full; path = $candidate }) -AllowFailure
    $names = if ($probe.Json) { @($probe.Json.Items | ForEach-Object Name) } else { @() }
    Write-Host "LocalState listing via path='$candidate': HTTP $($probe.Status); files: $($names -join ', ')"
    if ($probe.Status -ne 200) { Fail "LocalState path '$candidate' did not list (HTTP $($probe.Status))" }
    $cfg | Add-Member -NotePropertyName LocalStatePath -NotePropertyValue $candidate -Force
    Save-HarnessConfig $cfg
    Write-Host "Saved LocalStatePath='$candidate' to config."
}

function Invoke-Prepare {
    $cfg = Get-HarnessConfig
    if (-not $Package) { Fail '-Package is required' }
    $info = Get-PackageInfo $Package
    $idProblems = Test-ExpectedIdentity $info $cfg
    $sym = Test-Symbols $info
    $hashes = Test-ReleaseHashes $Package
    $script:RunDir = New-RunDir $cfg.RunsRoot $Label
    $run = [ordered]@{
        harness_version = $script:HarnessVersion
        harness_sha256 = (Get-FileHash $PSCommandPath -Algorithm SHA256).Hash
        label = $Label; created = (Get-Date).ToString('o'); dryrun = $script:DryRun
        visual = [bool]$Visual; confirmation_required = [bool]$Visual
        console = $cfg.ConsoleUrl; tls_sha256 = $cfg.PinnedCertSha256
        package = $info; symbols = $sym; release_hashes = $hashes; identity_problems = $idProblems
        deploy = $null; launch = $null; stop = $null; collect = $null
    }
    Save-RunJson $script:RunDir $run
    "# Vitox run $Label`n`nCreated $($run.created). Package $($info.PackageFullName) ($($info.Size) B, SHA-256 $($info.Sha256)).`n" |
        Set-Content (Join-Path $script:RunDir 'summary.md') -Encoding utf8
    Write-Host "Run dir     : $script:RunDir"
    Write-Host "Package     : $($info.PackageFullName)  $($info.Size) B  SHA-256 $($info.Sha256)"
    Write-Host "PFN         : $($info.PackageFamilyName)  caps: $($info.Capabilities -join ',')"
    Write-Host "Symbols     : $($sym.Reason)"
    Write-Host "Release sums: $(if ($hashes.Ok) { 'OK' } else { $hashes.Mismatches -join '; ' })"
    if ($idProblems.Count) { Fail ("identity check failed:`n  " + ($idProblems -join "`n  ")) }
    if (-not $sym.Match) { Fail "symbols check failed: $($sym.Reason)" }
    if (-not $hashes.Ok) { Fail "release hash check failed: $($hashes.Mismatches -join '; ')" }

    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 300 # 0.1.31: the pre-snapshot downloads large diagnostic logs (core_log ~25 MB) that exceed 30 s
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    $all = Get-InstalledIdentities $pk.Json
    Write-Json (Select-VitoxPackages $pk.Json $cfg) (Join-Path $script:RunDir 'pre\packages.json')
    $running = Stop-RunningVitox $cfg ([bool]$KeepRunningDiagnostic)
    $r0 = Get-RunJson $script:RunDir
    $r0 | Add-Member pre_running ([pscustomobject]$running) -Force
    $r0 | Add-Member keep_running_diagnostic ([bool]$KeepRunningDiagnostic) -Force
    Save-RunJson $script:RunDir $r0
    $set = Invoke-Wdp -Method GET -Path '/ext/settings' -AllowFailure
    if ($set.Json) { Write-Json $set.Json (Join-Path $script:RunDir 'pre\settings.json') }
    $reduced = Reduce-StaleForDecision $info $all ([bool]$AllowStaleUserRegistration)
    if ($reduced.Reduced) { Write-Host "Stale       : older per-user registration(s) ignored for the deploy decision (-AllowStaleUserRegistration): $($reduced.Stale -join ', ')" }
    $decision = Get-UpdateDecision $info $reduced.Installed (Get-Ledger)
    $installed = @($reduced.Installed | Where-Object { $_.Name -ceq $info.Name })
    if ($installed.Count -eq 1 -and $cfg.LocalStatePath) {
        $saved = Save-LocalStateLogs $cfg $installed[0].PackageFullName (Join-Path $script:RunDir 'pre')
        Write-Host "Pre-snapshot: $($saved -join ', ')"
    }
    $run = Get-RunJson $script:RunDir
    $run.deploy = [ordered]@{ decision = $decision; requested = [bool]$Deploy }
    Write-Host "Deploy      : $($decision.Action) - $($decision.Reason)"
    if (-not $decision.Allowed) { Save-RunJson $script:RunDir $run; Fail "deploy policy refused: $($decision.Reason)" }

    if ($Deploy -and $decision.Action -in 'install', 'update') {
        $files = @($info.Path)
        foreach ($dep in $info.Dependencies) {
            $have = @($all | Where-Object { $_.Name -eq $dep.Name -and $_.PackageFullName -match '_x64_' } | Where-Object {
                    [version]("{0}.{1}.{2}.{3}" -f $_.Version.Major, $_.Version.Minor, $_.Version.Build, $_.Version.Revision) -ge [version]$dep.MinVersion })
            if ($have.Count) { Write-Host "Dependency  : $($dep.Name) >= $($dep.MinVersion) already on console"; continue }
            $depFile = Get-ChildItem (Join-Path (Split-Path $info.Path) 'Dependencies\x64') -Filter '*.appx' -ErrorAction SilentlyContinue | Where-Object Name -like "$($dep.Name -replace '\.140\.00$','').x64*" | Select-Object -First 1
            if (-not $depFile) { Fail "dependency $($dep.Name) not on console and no x64 package under Dependencies\x64" }
            Write-Host "Dependency  : uploading $($depFile.Name)"
            $files += $depFile.FullName
        }
        $run.deploy.files = $files
        Connect-Console $cfg 900
        $r = Invoke-Wdp -Method POST -Path '/api/app/packagemanager/package' -Query ([ordered]@{ package = $info.FileName }) -Content ([VitoxHarness.MultipartFiles]::new([string[]]$files))
        if ($r.DryRun) {
            $run.deploy.result = 'dryrun'
        } else {
            Connect-Console $cfg 30
            # A 200 seen before any 204 may be the previous deployment's result, so presence is the real check.
            $deadline = (Get-Date).AddMinutes(10); $final = $null; $seenRunning = $false; $polls = @()
            do {
                Start-Sleep -Milliseconds 1000
                $s = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/state' -AllowFailure
                $polls += $s.Status
                if ($s.Status -eq 204) { $seenRunning = $true }
                if ($s.Status -eq 200 -and $seenRunning) { $final = $s.Json; break }
            } while ((Get-Date) -lt $deadline -and ($s.Status -in 200, 204))
            $run.deploy.state = $final; $run.deploy.state_polls = $polls
            $ok = $false; $after = @(); $sel = $null
            for ($i = 0; $i -lt 15 -and -not $ok; $i++) {
                $after = Get-InstalledVitox $cfg
                $ok = $after.Count -eq 1 -and $after[0].PackageFullName -ceq $info.PackageFullName -and $after[0].Publisher -ceq $info.Publisher
                if (-not $ok) { Start-Sleep -Seconds 2 }
            }
            # Xbox per-user registration: the update can land for DefaultAccount while the signed-in user
            # keeps the older version listed. That is an install of the new package, not a failure.
            $staleOnly = $false
            if (-not $ok -and $final -and $final.Success) {
                $sel = Select-LaunchTarget $after $info $true
                $staleOnly = $sel.Ok -and $sel.Entry.Publisher -ceq $info.Publisher
            }
            if ($final -and -not $final.Success) { $ok = $false }
            $run.deploy.result = if ($ok) { 'installed' } elseif ($staleOnly) { 'installed-with-stale-user-registration' } else { 'FAILED' }
            if ($staleOnly) {
                $ok = $true
                $run.deploy.stale_registrations = @($sel.Stale)
                Write-Host "Install     : new package installed; older per-user registration still listed: $($sel.Stale -join ', ') (launch needs -AllowStaleUserRegistration)"
            }
            $run.deploy.installed_after = @($after | Select-Object -Property * -ExcludeProperty Raw)
            if ($ok) { Add-LedgerEntry $info $script:RunDir }
            Write-Host "Install     : $($run.deploy.result) state=$($final | ConvertTo-Json -Compress)"
            if (-not $ok) { Save-RunJson $script:RunDir $run; Fail 'package not installed as expected after deploy' }
        }
    } elseif ($decision.Action -eq 'none') {
        $run.deploy.result = 'already-installed'
    } else {
        $run.deploy.result = 'not-requested'
    }
    Save-RunJson $script:RunDir $run
    if ($Visual) {
        # A visual run must never silently rely on -AllowStaleUserRegistration: surface it before the gate.
        if (-not $script:DryRun) {
            $now = Get-InstalledVitox $cfg
            $stale = Select-LaunchTarget $now $info $true
            if ($stale.Ok -and $stale.Stale.Count) {
                $run.deploy.visual_stale_registrations = @($stale.Stale)
                Save-RunJson $script:RunDir $run
                Write-Host "WARNING     : stale per-user registration(s) present: $($stale.Stale -join ', '). A visual launch then needs -AllowStaleUserRegistration explicitly; the session version is verified at collect."
            }
        }
        New-Gate $script:RunDir
        Write-Host "Visual run: gate armed in $(Join-Path $script:RunDir 'gate.json'). Launch requires the user's literal reply."
        Write-Host 'Please confirm'
    }
}

function Resolve-RunDir {
    if (-not $Run) { Fail '-Run <run dir> is required' }
    $d = (Resolve-Path $Run).Path
    if (-not (Test-Path (Join-Path $d 'run.json'))) { Fail "not a run dir: $d" }
    $script:RunDir = $d
    return $d
}

function Invoke-Launch {
    $cfg = Get-HarnessConfig
    $dir = Resolve-RunDir
    $run = Get-RunJson $dir
    if ($run.visual) {
        $g = Test-Gate $dir $Confirmation
        if (-not $g.Ok) { Fail "visual gate: $($g.Reason). Launch refused." }
    }
    Connect-Console $cfg
    $installedAll = Get-InstalledVitox $cfg
    $sel = Select-LaunchTarget $installedAll $run.package ([bool]$AllowStaleUserRegistration)
    if (-not $sel.Ok) { Fail "$($sel.Reason); launch refused" }
    $inst = @($sel.Entry)
    if ($sel.Stale.Count) { Write-Host "Launch      : $($sel.Reason)" }
    $full = $run.package.PackageFullName
    # WDP 'appid' is the PRAID as reported by the package list (on Xbox: the AUMID '<PFN>!<AppId>');
    # 'App' alone is rejected with HTTP 400 / 0x8D160120. Fail closed unless it is exactly the expected AUMID.
    $aumid = [string]$inst[0].AppUserModelId
    $expectedAumid = "$($cfg.Expected.PackageFamilyName)!$($cfg.Expected.AppId)"
    if ($aumid -cne $expectedAumid) { Fail "installed PackageRelativeId '$aumid' != expected '$expectedAumid'; launch refused" }
    $pre = $null
    if ($cfg.LocalStatePath) { $pre = Get-LocalStateFiles $cfg $full }
    $preMem = $null  # tick count baseline is taken from the first poll
    if ($run.visual -and -not $script:DryRun) { Set-GateConfirmed $dir $Confirmation }
    $t = Get-Date
    $r = Invoke-Wdp -Method POST -Path '/api/taskmanager/app' -Query ([ordered]@{ appid = (ConvertTo-Hex64 $aumid); package = (ConvertTo-Hex64 $full) })
    $launch = [ordered]@{ requested_at = $t.ToString('o'); method = 'POST /api/taskmanager/app'; appid = $aumid; dryrun = $r.DryRun; status = $r.Status; pre_mem_log_size = $preMem; polls = @()
        stale_registrations = @($sel.Stale); requires_version_check = [bool]$sel.Stale.Count }
    if (-not $r.DryRun) {
        $poll = Join-Path $dir 'xbox_poll.jsonl'
        $last = $preMem
        $end = (Get-Date).AddSeconds($MonitorSec)
        while ((Get-Date) -lt $end) {
            Start-Sleep -Seconds 3
            $p = [ordered]@{ t = (Get-Date).ToString('o') }
            if (-not $cfg.LocalStatePath) {
                $found = Find-LocalStatePath $cfg $full
                if ($found) { Save-LocalStatePath $cfg $found; $launch.localstate_path_discovered = $found; Write-Host "LocalState  : discovered path '$found' (saved to config)" }
            }
            # Heartbeat = number of 'tick' lines in a fresh download of mem_log.txt. (The listing's FileSize is
            # stale (0) while the app holds the file open, observed on Xbox, so sizes are not used.)
            $ticks = $null
            if ($cfg.LocalStatePath) {
                $tmpLog = Join-Path $dir 'poll_mem_log.tmp'
                $q = [ordered]@{ knownfolderid = 'LocalAppData'; packagefullname = $full; filename = 'mem_log.txt'; path = $cfg.LocalStatePath }
                $dl = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/file' -Query $q -OutFile $tmpLog -AllowFailure
                if ($dl.Status -eq 200) { $ticks = @(Select-String -Path $tmpLog -Pattern '\] tick ').Count }
                Remove-Item $tmpLog -ErrorAction SilentlyContinue
            }
            $p.mem_log_ticks = $ticks
            $p.heartbeat_growing = ($null -ne $ticks) -and (($null -eq $last) -or ($ticks -gt $last))
            $last = $ticks
            $size = $ticks
            $pr = Invoke-Wdp -Method GET -Path '/api/resourcemanager/processes' -AllowFailure
            $p.processes_http = $pr.Status
            if ($pr.Json) {
                $vp = @($pr.Json.Processes) | Where-Object { [string]$_.ImageName -imatch '^Vitox(M1Probe)?(\.exe)?$' } | Select-Object -First 1
                $p.process_seen = [bool]$vp
                # 0.1.25 (M9A.6 diagnostic, read-only): the process record itself, to tell a busy, idle or suspended process apart
                if ($vp) { $p.proc = [ordered]@{ cpu = $vp.CPUUsage; private = $vp.PrivateWorkingSet; ws = $vp.WorkingSetSize; commit = $vp.TotalCommit; pagefile = $vp.PageFileUsage; running = $vp.IsRunning } }
            }
            ($p | ConvertTo-Json -Compress) | Add-Content $poll -Encoding utf8
            $launch.polls += [pscustomobject]$p
            Write-Host ("poll {0}: ticks={1} growing={2} proc={3}" -f $p.t.Substring(11, 8), $size, $p.heartbeat_growing, $(if ($p.Contains('process_seen')) { $p.process_seen } else { "n/a(HTTP $($pr.Status))" }))
        }
        $grow = @($launch.polls | Where-Object heartbeat_growing).Count
        $lastTwo = @($launch.polls | Select-Object -Last 2)
        $launch.behavior = if ($grow -eq 0) { 'no-heartbeat (did not start, or logs unreadable)' }
        elseif (@($lastTwo | Where-Object heartbeat_growing).Count -eq 0) { 'heartbeat-stopped (exited or suspended during monitor window)' }
        else { 'alive (heartbeat growing)' }
        Write-Host "Behavior    : $($launch.behavior)"
    }
    $run.launch = $launch
    Save-RunJson $dir $run
}

function Invoke-Stop {
    $cfg = Get-HarnessConfig
    $dir = Resolve-RunDir
    $run = Get-RunJson $dir
    Connect-Console $cfg
    $r = Invoke-Wdp -Method DELETE -Path '/api/taskmanager/app' -Query ([ordered]@{ package = (ConvertTo-Hex64 $run.package.PackageFullName) })
    $run.stop = [ordered]@{ at = (Get-Date).ToString('o'); dryrun = $r.DryRun; status = $r.Status }
    Save-RunJson $dir $run
    Write-Host "Stop        : $(if ($r.DryRun) { 'dry run' } else { "HTTP $($r.Status)" })"
}

function Invoke-Collect {
    $cfg = Get-HarnessConfig
    $dir = Resolve-RunDir
    $run = Get-RunJson $dir
    if (-not $cfg.LocalStatePath) { Fail 'LocalStatePath unknown; run discover first' }
    Connect-Console $cfg 300 # 0.1.30: large diagnostic logs (m8a_stderr) exceed 30 s
    $full = $run.package.PackageFullName
    $post = Join-Path $dir 'post'
    $saved = Save-LocalStateLogs $cfg $full $post
    # Sessions already present before this run (pre snapshot) are excluded. 0.1.7: by POSITION, not PID:
    # the LocalState logs only grow (pre is a prefix of post) and the console reuses PIDs (run 20261018-175218).
    $prePids = @()
    $preCount = 0
    $preLife = Join-Path $dir 'pre\lifecycle_log.txt'
    if (Test-Path $preLife) { $preSessions = Split-Sessions (Get-Content $preLife); $prePids = @($preSessions | ForEach-Object Pid); $preCount = $preSessions.Count }
    $postLife = Join-Path $post 'lifecycle_log.txt'
    $sessions = if (Test-Path $postLife) { Split-Sessions (Get-Content $postLife) } else { @() }
    $new = @(Select-NewSessions $sessions $preCount)
    $target = if ($new.Count) { $new[-1] } else { $null }
    $logs = Join-Path $dir 'logs'
    if ($target) {
        foreach ($f in $saved) {
            $s = Split-Sessions (Get-Content (Join-Path $post $f))
            $match = @($s | Where-Object Pid -eq $target.Pid) | Select-Object -Last 1
            if ($match) { $match.Lines | Set-Content (Join-Path $logs $f) -Encoding utf8 }
        }
    }
    $crash = Join-Path $post 'crash_log.txt'
    if ((Test-Path $crash) -and (Get-Item $crash).Length -gt 0) { Copy-Item $crash (Join-Path $dir 'crash\crash_log.txt') }
    $dumps = Invoke-Wdp -Method GET -Path '/api/debug/dump/usermode/dumps' -AllowFailure
    $wer = Invoke-Wdp -Method GET -Path '/api/wer/reports' -AllowFailure
    $vd = Select-VitoxDumps $dumps.Json $cfg $run.package.Executable
    $vw = Select-VitoxWer $wer.Json $cfg $run.package.Executable
    if ($dumps.Json) { Write-Json $vd (Join-Path $dir 'crash\dumps_list.json') }
    if ($wer.Json) { Write-Json $vw (Join-Path $dir 'crash\wer_reports.json') }
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    Write-Json (Select-VitoxPackages $pk.Json $cfg) (Join-Path $post 'packages.json')
    $analysis = if ($target) { Get-SessionAnalysis $logs } else { $null }
    # The session must have run the run's package version (the app logs its own package identity).
    $sessionVer = if ($analysis -and $analysis.PSObject.Properties['package_version']) { [string]$analysis.package_version } else { $null }
    $verOk = ($sessionVer -eq [string]$run.package.Version)
    $staleLaunch = $run.launch -and $run.launch.PSObject.Properties['requires_version_check'] -and $run.launch.requires_version_check
    $validity = if ($verOk) { 'VALID' } elseif ($staleLaunch) { 'INVALID (session did not run the run package version)' } else { 'UNVERIFIED (session version missing or different)' }
    if ($staleLaunch -and -not $verOk) {
        # Not a test result: the stale registration ran. Keep the raw logs, drop the analysis.
        $analysis = $null
    }
    $run.collect = [ordered]@{ at = (Get-Date).ToString('o'); files = $saved; sessions_in_log = @($sessions | ForEach-Object Pid); pre_session_pids = $prePids
        session_pid = $(if ($target) { $target.Pid } else { $null }); dumps_http = $dumps.Status; wer_http = $wer.Status
        session_version = $sessionVer; expected_version = [string]$run.package.Version; version_verified = $verOk; validity = $validity; analysis = $analysis }
    Save-RunJson $dir $run
    $md = @("# Vitox run $($run.label)", '', "- Package: ``$($run.package.PackageFullName)`` ($($run.package.Size) B, SHA-256 ``$($run.package.Sha256)``)",
        "- Visual: $($run.visual); gate: $(if (Test-Path (Join-Path $dir 'gate.json')) { (Read-Json (Join-Path $dir 'gate.json')).state } else { 'n/a' })",
        "- Launch: $(if ($run.launch) { "$($run.launch.method) HTTP $($run.launch.status); $($run.launch.behavior)" } else { 'none' })",
        "- Session pid: $(if ($target) { $target.Pid } else { 'NOT FOUND (app may not have started)' })",
        "- Session version: $sessionVer (expected $($run.package.Version)) -> **$validity**", '')
    if ($analysis) { $md += '```json'; $md += ($analysis | ConvertTo-Json -Depth 6); $md += '```' }
    $md | Set-Content (Join-Path $dir 'summary.md') -Encoding utf8
    Write-Host "Collected   : $($saved -join ', ')"
    Write-Host "Session pid : $(if ($target) { $target.Pid } else { 'none found' }) (pre-existing: $($prePids -join ','))"
    Write-Host "Summary     : $(Join-Path $dir 'summary.md')"
    Write-Host "Version     : session=$sessionVer expected=$($run.package.Version) -> $validity"
    if ($staleLaunch -and -not $verOk) { exit 4 }
}

# ---------------------------------------------------------------- M8A homebrew root (DevelopmentFiles\Vitox\homebrew)

# param.sfo (PSF): returns key -> string/uint value
function Read-Sfo([byte[]]$B) {
    if ($B.Length -lt 20 -or $B[0] -ne 0 -or $B[1] -ne 0x50 -or $B[2] -ne 0x53 -or $B[3] -ne 0x46) { Fail 'param.sfo: bad magic' }
    $kt = [BitConverter]::ToUInt32($B, 8); $dt = [BitConverter]::ToUInt32($B, 12); $n = [BitConverter]::ToUInt32($B, 16)
    if ($n -gt 256 -or 20 + 16 * $n -gt $B.Length) { Fail 'param.sfo: bad entry count' }
    $r = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $e = 20 + 16 * $i
        $ko = [BitConverter]::ToUInt16($B, $e); $fmt = [BitConverter]::ToUInt16($B, $e + 2)
        $len = [BitConverter]::ToUInt32($B, $e + 4); $do = [BitConverter]::ToUInt32($B, $e + 12)
        $ks = $kt + $ko; $ke = $ks
        while ($ke -lt $B.Length -and $B[$ke] -ne 0) { $ke++ }
        $key = [Text.Encoding]::ASCII.GetString($B, $ks, $ke - $ks)
        $ds = $dt + $do
        if ($ds + $len -gt $B.Length) { continue }
        if ($fmt -eq 0x0404) { $r[$key] = [BitConverter]::ToUInt32($B, $ds) }
        else { $r[$key] = [Text.Encoding]::UTF8.GetString($B, $ds, $len).TrimEnd([char]0) }
    }
    return $r
}

function Get-DevFiles([string]$P) {
    $r = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/files' -Query ([ordered]@{ knownfolderid = 'DevelopmentFiles'; path = $P }) -AllowFailure
    if ($r.Status -ne 200) { return $null }
    return @(if ($r.Json) { $r.Json.Items } else { @() })
}
function Get-DevFileBytes([string]$P, [string]$Name) {
    $tmp = [IO.Path]::GetTempFileName()
    try {
        $r = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/file' -Query ([ordered]@{ knownfolderid = 'DevelopmentFiles'; filename = $Name; path = $P }) -OutFile $tmp -AllowFailure
        if ($r.Status -ne 200) { return $null }
        return [IO.File]::ReadAllBytes($tmp)
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
}
function Test-DevItemIsFolder($Item) { return ([int]$Item.Type -band 16) -ne 0 }

# every app folder in the root with its param.sfo TITLE_ID (read-only); in app-staging the licenses folder is not an app
function Get-RemoteHomebrew([string]$Root = $script:HomebrewRoot) {
    $items = Get-DevFiles $Root
    if ($null -eq $items) { return $null }
    $staging = $Root -ceq $script:AppStagingRoot
    $apps = foreach ($it in @($items | Where-Object { (Test-DevItemIsFolder $_) -and -not ($staging -and $_.Name -ceq $script:LicenseFolder) })) {
        $p = "$Root\$($it.Name)"
        $files = Get-DevFiles $p
        $eboot = @($files | Where-Object { $_.Name -ceq 'eboot.bin' }) | Select-Object -First 1
        $sfoB = Get-DevFileBytes "$p\sce_sys" 'param.sfo'
        $id = $null; $title = $null; $why = $null
        if (-not $eboot) { $why = 'no eboot.bin' }
        elseif (-not $sfoB) { $why = 'no sce_sys\param.sfo' }
        else {
            try { $s = Read-Sfo $sfoB; $id = $s['TITLE_ID']; $title = $s['TITLE'] } catch { $why = $_.Exception.Message }
            # M9A.2C: an app-staging folder is always named after its TITLE_ID (app-push); anything else is not selectable
            if (-not $why -and $staging -and $it.Name -cne $id) { $why = "app-staging folder name is not its param.sfo TITLE_ID '$id'" }
        }
        [pscustomobject]@{ Folder = $it.Name; TitleId = $id; Title = $title; EbootSize = $(if ($eboot) { $eboot.FileSize } else { 0 }); Reject = $why }
    }
    return @($apps)
}

function Send-DevFolder([string]$P, [string]$Name) {
    $r = Invoke-Wdp -Method POST -Path '/api/filesystem/apps/folder' -Query ([ordered]@{ knownfolderid = 'DevelopmentFiles'; path = $P; newfoldername = $Name }) -AllowFailure
    return $r
}
function Send-DevFile([string]$P, [string]$LocalFile, [string]$Name) {
    if (-not (Test-HomebrewName $Name)) { Fail "refusing to upload '$Name': not a plain file name" }
    # the same multipart writer as the package install (quoted name/filename; WDP rejects .NET's default form) - the
    # uploaded file takes the local file's name, so it must equal $Name
    if ([IO.Path]::GetFileName($LocalFile) -cne $Name) { Fail "upload name '$Name' differs from the local file name '$LocalFile'" }
    $mp = [VitoxHarness.MultipartFiles]::new([string[]]@($LocalFile))
    return Invoke-Wdp -Method POST -Path '/api/filesystem/apps/file' -Query ([ordered]@{ knownfolderid = 'DevelopmentFiles'; path = $P }) -Content $mp
}
function Confirm-DevFolder([string]$Parent, [string]$Name) {
    $items = Get-DevFiles $Parent
    if ($items -and @($items | Where-Object { $_.Name -ceq $Name -and (Test-DevItemIsFolder $_) }).Count) { return 'exists' }
    $r = Send-DevFolder $Parent $Name
    if ($r.DryRun) { return 'dryrun' }
    $items = Get-DevFiles $Parent
    if (-not ($items -and @($items | Where-Object { $_.Name -ceq $Name -and (Test-DevItemIsFolder $_) }).Count)) {
        Fail "could not create folder '$Name' in DevelopmentFiles$Parent (HTTP $($r.Status) $($r.Body))"
    }
    return "created (HTTP $($r.Status))"
}

# 0.1.18 (M9A.5 reliability): read-only backup of one title's guest save data + the guest trophy tree to the PC.
# Console: GET only (listing + file download). PC: a NEW timestamped folder (refused if it exists); nothing is overwritten.
# 0.1.29 (user-requested 2026-10-29): uninstall leftover Vitox package registrations (after a 0x800703F8 damaged-hive
# deploy). Only full names of the expected identity (Vitox.M1Probe_<version>_x64__<publisher id>) that the console lists
# as installed; any other package is refused. Package data in DevelopmentFiles (vita-fs: saves, apps) is untouched.
function Invoke-Uninstall {
    $cfg = Get-HarnessConfig
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 120
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    $all = Get-InstalledIdentities $pk.Json # returns the list as ONE object: assign first, then pipe (unrolls)
    $vitox = @($all | Where-Object { $_.Name -ceq $cfg.Expected.Name })
    $rx = '^' + [regex]::Escape($cfg.Expected.Name) + '_[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+_x64__[a-z0-9]{13}$'
    if (-not $vitox.Count) { Write-Host 'Uninstall   : no Vitox package is installed'; return }
    foreach ($v in $vitox) {
        $full = [string]$v.PackageFullName
        if ($full -cnotmatch $rx) { Fail "refusing '$full' (not a Vitox full name)" }
        $q = [ordered]@{ package = $full }
        $r = Invoke-Wdp -Method DELETE -Path '/api/app/packagemanager/package' -Query $q -AllowFailure
        Write-Host ("Uninstall   : {0} -> HTTP {1} {2}" -f $full, $r.Status, $(if ($r.Body) { $r.Body.Substring(0, [Math]::Min(200, $r.Body.Length)) } else { '' }))
    }
    $pk = Invoke-Wdp -Method GET -Path '/api/app/packagemanager/packages'
    $all = Get-InstalledIdentities $pk.Json
    $left = @($all | Where-Object { $_.Name -ceq $cfg.Expected.Name })
    Write-Host ("Remaining   : {0} Vitox package(s){1}" -f $left.Count, $(if ($left.Count) { ': ' + (($left | ForEach-Object { $_.PackageFullName }) -join ', ') } else { '' }))
}

# 0.1.28: READ-ONLY. Save the console's current screen (Device Portal media capture) as a PNG under RunsRoot\SCREENSHOTS.
function Invoke-Screenshot {
    $cfg = Get-HarnessConfig
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 60
    $dir = Join-Path $cfg.RunsRoot 'SCREENSHOTS'
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $out = Join-Path $dir ("{0}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $q = [ordered]@{ download = 'true' }
    $r = Invoke-Wdp -Method GET -Path '/ext/screenshot' -Query $q -OutFile $out
    if (-not (Test-Path -LiteralPath $out)) { Fail "screenshot: HTTP $($r.Status), nothing saved" }
    Write-Host ("Screenshot  : {0} ({1} B)" -f $out, (Get-Item -LiteralPath $out).Length)
}

function Invoke-DataBackup {
    $cfg = Get-HarnessConfig
    if ($TitleId -cnotmatch '^[A-Z0-9]{9}$') { Fail '-TitleId <9-character TITLE_ID> is required' }
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 120
    $user = '\Vitox\vita-fs\ux0\user\00'
    $sets = [ordered]@{ "savedata\$TitleId" = "$user\savedata\$TitleId"; 'trophy' = "$user\trophy" }
    $dest = Join-Path (Join-Path (Split-Path -Parent $cfg.RunsRoot) 'DATA-BACKUPS') ("{0}_{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $TitleId)
    if (Test-Path -LiteralPath $dest) { Fail "$dest already exists (backups are never overwritten)" }
    New-Item -ItemType Directory -Path $dest | Out-Null
    $manifest = [Collections.Generic.List[string]]::new()
    $bad = 0; $n = 0; $bytes = 0
    foreach ($set in $sets.Keys) {
        $remote = $sets[$set]
        $t = Get-RemoteTree $remote
        Write-Host "Set         : $set <- DevelopmentFiles$remote ($($t.Count) file(s))"
        foreach ($k in @($t.Keys | Sort-Object)) {
            $rel = "$set\$k"
            $d = Split-Path -Parent "$remote\$k"
            $data = Get-DevFileBytes $d $t[$k].Name
            $local = Join-Path $dest $rel
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $local) | Out-Null
            if ($null -eq $data) { Write-Host "  DOWNLOAD FAILED  $rel"; $bad++; continue }
            [IO.File]::WriteAllBytes($local, $data)
            $sha = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
            $sizeOk = [int64]$data.Length -eq [int64]$t[$k].FileSize
            # a second download must hash the same (the file is not being written while it is copied)
            $again = Get-DevFileSha256 $d $t[$k].Name
            $ok = $sizeOk -and $again -ceq $sha
            if (-not $ok) { $bad++ }
            $n++; $bytes += $data.Length
            $manifest.Add(("{0}  {1}  {2}{3}" -f $sha, $data.Length, $rel, $(if ($ok) { '' } else { "  MISMATCH(listed $($t[$k].FileSize), re-read $again)" })))
            Write-Host ("  {0}  {1,10} B  {2}{3}" -f $sha, $data.Length, $rel, $(if ($ok) { '' } else { '  MISMATCH' }))
        }
    }
    [IO.File]::WriteAllLines((Join-Path $dest 'MANIFEST.sha256.txt'), $manifest)
    Write-Host "Backup      : $dest | $n file(s), $bytes B | $(if ($bad) { "$bad PROBLEM(S)" } else { 'all verified (size + SHA-256, read twice)' })"
    if ($bad) { exit 2 }
}

function Invoke-HomebrewList {
    $cfg = Get-HarnessConfig
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg
    # 0.1.17 (M9A.3 D): -AppRoot app-staging lists every staged file with size + SHA-256 (read-only evidence)
    if ($AppRoot -ceq 'app-staging') {
        $t = Get-RemoteTree $script:AppStagingRoot
        Write-Host "App-staging root: DevelopmentFiles$($script:AppStagingRoot) - $($t.Count) file(s)"
        foreach ($k in @($t.Keys | Sort-Object)) {
            $d = Split-Path -Parent "$($script:AppStagingRoot)\$k"
            Write-Host ("  {0}  {1,10} B  {2}" -f (Get-DevFileSha256 $d $t[$k].Name), $t[$k].FileSize, $k)
        }
        # the launcher's guest ux0 (vita-fs): app folders by name, every license file (read-only ux0 diff evidence)
        $ua = @(Get-DevFiles '\Vitox\vita-fs\ux0\app' | Where-Object { $_ } | ForEach-Object Name | Sort-Object)
        Write-Host "ux0:app ($($ua.Count)): $($ua -join ' ')"
        $ul = Get-RemoteTree '\Vitox\vita-fs\ux0\license'
        Write-Host "ux0:license files ($($ul.Count)): $(@($ul.Keys | Sort-Object) -join ' ')"
        return
    }
    $apps = Get-RemoteHomebrew
    if ($null -eq $apps) { Write-Host "Homebrew root: DevelopmentFiles$($script:HomebrewRoot) does not exist yet (homebrew-push creates it)"; return }
    Write-Host "Homebrew root: DevelopmentFiles$($script:HomebrewRoot) - $(@($apps).Count) folder(s)"
    foreach ($a in $apps) {
        if ($a.Reject) { Write-Host ("  {0,-24} REJECT: {1}" -f $a.Folder, $a.Reject) }
        else { Write-Host ("  {0,-24} TITLE_ID={1} TITLE='{2}' eboot.bin {3} B" -f $a.Folder, $a.TitleId, $a.Title, $a.EbootSize) }
    }
    $sel = Get-DevFileBytes $script:HomebrewRoot 'vitox-launch.txt'
    if ($sel) { Write-Host "vitox-launch.txt:"; ([Text.Encoding]::UTF8.GetString($sel) -split "`n") | Where-Object { $_.Trim() } | ForEach-Object { Write-Host "  $($_.TrimEnd())" } }
    else { Write-Host 'vitox-launch.txt: absent (the launcher takes the single valid app, else refuses)' }
}

function Invoke-HomebrewPush {
    $cfg = Get-HarnessConfig
    if (-not $Source) { Fail '-Source <local extracted homebrew folder> is required' }
    $src = (Resolve-Path -LiteralPath $Source).Path
    $sfoPath = Join-Path $src 'sce_sys\param.sfo'
    if (-not (Test-Path -LiteralPath (Join-Path $src 'eboot.bin') -PathType Leaf)) { Fail "$src has no eboot.bin" }
    if (-not (Test-Path -LiteralPath $sfoPath -PathType Leaf)) { Fail "$src has no sce_sys\param.sfo" }
    $sfo = Read-Sfo ([IO.File]::ReadAllBytes($sfoPath))
    $id = [string]$sfo['TITLE_ID']
    if ($id -cnotmatch '^[A-Z0-9]{9}$') { Fail "param.sfo TITLE_ID '$id' is not 9 characters A-Z/0-9" }
    $name = if ($Folder) { $Folder } else { $id }
    if (-not (Test-HomebrewName $name)) { Fail "folder name '$name' is not a plain name" }
    $files = @(Get-ChildItem -LiteralPath $src -Recurse -File -Force)
    $total = ($files | Measure-Object Length -Sum).Sum
    if ($files.Count -gt 4000 -or $total -gt 512MB) { Fail "refusing: $($files.Count) files / $total B (limits 4000 / 512 MiB)" }
    $plan = foreach ($f in $files) {
        $rel = [IO.Path]::GetRelativePath($src, $f.FullName)
        $segs = $rel.Split([IO.Path]::DirectorySeparatorChar)
        foreach ($s in $segs) { if (-not (Test-HomebrewName $s)) { Fail "refusing: '$rel' has a segment that is not a plain name" } }
        $dir = if ($segs.Count -gt 1) { $segs[0..($segs.Count - 2)] -join '\' } else { '' } # (0..-1 would wrap in PowerShell)
        [pscustomobject]@{ Local = $f.FullName; Dir = $dir; Name = $segs[-1]; Size = $f.Length;
            Sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash }
    }
    Write-Host "Source      : $src -> DevelopmentFiles$($script:HomebrewRoot)\$name | TITLE_ID=$id TITLE='$($sfo['TITLE'])' | $($files.Count) file(s), $total B"
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 120
    $existing = Get-RemoteHomebrew
    if ($existing -and @($existing | Where-Object Folder -ceq $name).Count) { Write-Host "Target      : folder '$name' exists - files are uploaded over it (nothing is deleted)" }
    Write-Host "Root        : $(Confirm-DevFolder '\Vitox' 'homebrew')"
    Write-Host "App folder  : $(Confirm-DevFolder $script:HomebrewRoot $name)"
    $base = "$($script:HomebrewRoot)\$name"
    $made = @{}
    foreach ($d in @($plan | ForEach-Object Dir | Where-Object { $_ } | Sort-Object -Unique)) {
        $acc = $base
        foreach ($s in $d.Split('\')) {
            if (-not $made.ContainsKey("$acc\$s")) { $null = Confirm-DevFolder $acc $s; $made["$acc\$s"] = $true }
            $acc = "$acc\$s"
        }
    }
    foreach ($p in $plan) {
        $dir = if ($p.Dir) { "$base\$($p.Dir)" } else { $base }
        $r = Send-DevFile $dir $p.Local $p.Name
        Write-Host ("  upload {0}\{1} ({2} B, SHA-256 {3}) -> {4}" -f $dir, $p.Name, $p.Size, $p.Sha256.Substring(0, 16), $(if ($r.DryRun) { 'dryrun' } else { "HTTP $($r.Status)" }))
    }
    if ($script:DryRun) { return }
    $bad = 0
    foreach ($p in $plan) {
        $dir = if ($p.Dir) { "$base\$($p.Dir)" } else { $base }
        $it = @(Get-DevFiles $dir | Where-Object { $_.Name -ceq $p.Name }) | Select-Object -First 1
        if (-not $it -or [int64]$it.FileSize -ne [int64]$p.Size) { $bad++; Write-Host "  VERIFY FAIL: $dir\$($p.Name)" -ForegroundColor Red }
    }
    $eb = Get-DevFileBytes $base 'eboot.bin'
    $ebOk = $eb -and ([BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($eb)) -replace '-', '') -eq ($plan | Where-Object { -not $_.Dir -and $_.Name -ceq 'eboot.bin' }).Sha256
    Write-Host "Verify      : $($plan.Count - $bad)/$($plan.Count) files present with the same size | eboot.bin SHA-256 round-trip $(if ($ebOk) { 'OK' } else { 'MISMATCH' })"
    if ($bad -or -not $ebOk) { Fail 'homebrew-push verification failed' }
}

# one valid app by folder name, else by TITLE_ID; none or an ambiguous TITLE_ID fails closed
function Select-RemoteApp($Apps, [string]$Want) {
    $valid = @($Apps | Where-Object { -not $_.Reject })
    $byFolder = @($valid | Where-Object Folder -ceq $Want)
    $byId = @($valid | Where-Object TitleId -ceq $Want)
    if ($byFolder.Count -eq 1) { return [pscustomobject]@{ Hit = $byFolder[0]; Why = 'folder name' } }
    if ($byId.Count -eq 1) { return [pscustomobject]@{ Hit = $byId[0]; Why = 'TITLE_ID' } }
    $why = if ($byId.Count -gt 1) { "ambiguous: TITLE_ID $Want is in $($byId.Count) folders ($(($byId | ForEach-Object Folder) -join ', ')) - select by folder" } else { "no valid app folder or TITLE_ID '$Want'" }
    return [pscustomobject]@{ Hit = $null; Why = $why }
}

# M9A.3A: licensed_load=staged-license is written only together with its companions (the launcher enforces the same
# rule and fails closed otherwise). Empty = OK, else the reason.
function Get-LicensedLoadError([bool]$Licensed, [string]$Root, [bool]$Closed, [string]$Pres, [bool]$Diag) {
    if (-not $Licensed) { return '' }
    if ($Root -cne 'app-staging') { return '-LicensedLoad requires -AppRoot app-staging' }
    if (-not $Closed) { return '-LicensedLoad requires -FailClosed' }
    if ($Pres -cne 'headless') { return '-LicensedLoad requires -Presenter headless' }
    if ($Diag) { return '-LicensedLoad cannot be combined with -Trap or -ImeInput (diagnostic modes)' }
    return ''
}

function Invoke-HomebrewSelect {
    $cfg = Get-HarnessConfig
    if (-not $App) { Fail '-App <folder or TITLE_ID> is required' }
    # M8B.1: runtime-exact import discovery (DIAGNOSTIC only; the first call to an unselected NID stops the guest)
    if ($Trap -and $FailClosed) { Fail 'homebrew-select: -Trap and -FailClosed are exclusive' }
    $le = Get-LicensedLoadError ([bool]$LicensedLoad) $AppRoot ([bool]$FailClosed) $Presenter ([bool]$Trap -or [bool]$ImeInput)
    if ($le) { Fail "homebrew-select refused: $le" }
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg
    # M9A.2C: -AppRoot app-staging selects from DevelopmentFiles\Vitox\app-staging (vitox-launch.txt stays in homebrew)
    $root = if ($AppRoot -eq 'app-staging') { $script:AppStagingRoot } else { $script:HomebrewRoot }
    $s = Select-RemoteApp @(Get-RemoteHomebrew $root) $App
    if (-not $s.Hit) { Fail "homebrew-select refused (DevelopmentFiles$root): $($s.Why)" }
    $hit = $s.Hit
    $text = "# written by VitoxXbox.ps1 $($script:HarnessVersion) homebrew-select at $((Get-Date).ToString('o'))`napp=$($hit.Folder)`npresenter=$Presenter`ntimeout_s=$TimeoutSec`n"
    if ($Trap) { $text += "unsupported_imports=trap`n" }
    # M8E: the same runtime gate as an acceptance mode (a call to an unselected NID stops the guest and fails the run)
    if ($FailClosed) { $text += "unsupported_imports=fail-closed`n" }
    # M8C.3: the staged firmware for the PGF module only (launcher: firmware devices from firmware-staging, MANUAL [libpgf])
    if ($StagedPgf) { $text += "firmware=staging-pgf`n" }
    if ($SyncPipelines) { $text += "async_pipelines=off`n" }
    if ($AutoTouch) { $text += "auto_touch=$AutoTouch`n" }
    if ($AutoStick) { $text += "auto_stick=$AutoStick`n" }
    # M9A.2A: MANUAL lle-modules for app-bundled SDK runtime modules (launcher allowlist libc, libfios2)
    if ($LleModules) { $text += "lle_modules=$LleModules`n" }
    if ($AppRoot) { $text += "app_root=$AppRoot`n" }
    # M9A.3A: validation-only licensed-load request (this build validates the app + its staged license, then STOPs)
    if ($LicensedLoad) { $text += "licensed_load=staged-license`n" }
    # M8F.2A: DIAGNOSTIC scripted IME dialog input (launcher: ime_commit_text / submit_current_ime / dismiss_current_ime),
    # uploaded into the homebrew root as ime-input.txt
    if ($ImeInput) {
        if (-not (Test-Path -LiteralPath $ImeInput -PathType Leaf)) { Fail "-ImeInput file not found: $ImeInput" }
        $ir = Send-DevFile $script:HomebrewRoot (Resolve-Path -LiteralPath $ImeInput).Path 'ime-input.txt'
        if (-not $ir.DryRun) {
            $iback = Get-DevFileBytes $script:HomebrewRoot 'ime-input.txt'
            if (-not $iback -or -not [Linq.Enumerable]::SequenceEqual([byte[]]$iback, [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ImeInput).Path))) { Fail 'ime-input.txt read-back mismatch' }
        }
        $text += "ime_input=ime-input.txt`n"
    }
    # 0.1.32 (launcher 0.0.25.0): the library UI mode, its DIAGNOSTIC scripted buttons and the per-title profiles
    if ($Ui) { $text += "ui=$Ui`n" }
    foreach ($up in @(@{ Src = $UiScript; Name = 'ui-script.txt'; Key = 'ui_script' }, @{ Src = $Profiles; Name = 'vitox-profiles.txt'; Key = '' }, @{ Src = $SgdbKey; Name = 'steamgriddb-key.txt'; Key = '' })) {
        if (-not $up.Src) { continue }
        if (-not (Test-Path -LiteralPath $up.Src -PathType Leaf)) { Fail "file not found: $($up.Src)" }
        $full = (Resolve-Path -LiteralPath $up.Src).Path
        if ((Split-Path -Leaf $full) -cne $up.Name) {
            # Send-DevFile uploads under the local file name: stage a copy named as the target
            $stage = Join-Path ([IO.Path]::GetTempPath()) "vitox-up-$PID"
            New-Item -ItemType Directory -Force $stage | Out-Null
            Copy-Item -LiteralPath $full -Destination (Join-Path $stage $up.Name) -Force
            $full = Join-Path $stage $up.Name
        }
        $ur = Send-DevFile $script:HomebrewRoot $full $up.Name
        if (-not $ur.DryRun) {
            $uback = Get-DevFileBytes $script:HomebrewRoot $up.Name
            if (-not $uback -or -not [Linq.Enumerable]::SequenceEqual([byte[]]$uback, [IO.File]::ReadAllBytes($full))) { Fail "$($up.Name) read-back mismatch" }
        }
        if ($up.Key) { $text += "$($up.Key)=$($up.Name)`n" }
        Write-Host "Uploaded    : homebrew\$($up.Name)"
    }
    $tmp = Join-Path ([IO.Path]::GetTempPath()) 'vitox-launch.txt'
    [IO.File]::WriteAllText($tmp, $text, [Text.UTF8Encoding]::new($false))
    try { $r = Send-DevFile $script:HomebrewRoot $tmp 'vitox-launch.txt' } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    if ($r.DryRun) { Write-Host "[DRYRUN] would write vitox-launch.txt: app=$($hit.Folder) presenter=$Presenter timeout_s=$TimeoutSec"; return }
    $back = Get-DevFileBytes $script:HomebrewRoot 'vitox-launch.txt'
    $ok = $back -and [Text.Encoding]::UTF8.GetString($back) -ceq $text
    Write-Host "Selected    : DevelopmentFiles$root folder '$($hit.Folder)' TITLE_ID=$($hit.TitleId) TITLE='$($hit.Title)' | presenter=$Presenter timeout_s=$TimeoutSec$(if ($Trap) { ' | unsupported_imports=trap (M8B.1 diagnostic)' })$(if ($FailClosed) { ' | unsupported_imports=fail-closed (M8E)' })$(if ($ImeInput) { ' | ime_input=ime-input.txt (M8F.2A diagnostic)' }) | vitox-launch.txt read-back $(if ($ok) { 'OK' } else { 'MISMATCH' })"
    if (-not $ok) { Fail 'vitox-launch.txt read-back mismatch' }
}

# ---------------------------------------------------------------- M8C.1 firmware staging inputs (DevelopmentFiles\Vitox\firmware-staging)

# The ONLY files firmware-push will upload: the three user-supplied PUPs approved for M8C.1, by exact SHA-256.
# They are uploaded under space-free names (the WDP name rule) into \Vitox\firmware-staging\input.
$script:FirmwareInputs = @(
    [pscustomobject]@{ Source = 'PSVUPDAT.PUP'; Target = 'PSVUPDAT.PUP'; Sha256 = '6EF6DC8DA6DB026F28647713E473486D770087A605C52A8D751BFCA7478386CF' },
    [pscustomobject]@{ Source = 'Pre-install Firmware.PUP'; Target = 'PREINST.PUP'; Sha256 = '339D1439EB329CFBD1A936F0A1563458E0555EF52975486B2844E24B6049E33E' },
    [pscustomobject]@{ Source = 'Firmware Font Package.PUP'; Target = 'FONTPKG.PUP'; Sha256 = 'F372D306A7F2711125BEA29AA75CBCB1EB19724E9648478FBA07B8D655B37344' }
)

function Invoke-FirmwarePush {
    $cfg = Get-HarnessConfig
    $dl = Join-Path $env:USERPROFILE 'Downloads'
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('vitox-fw-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $tmp
    try {
        $staged = foreach ($f in $script:FirmwareInputs) {
            $src = Join-Path $dl $f.Source
            if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { Fail "missing $src" }
            $h = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
            if ($h -cne $f.Sha256) { Fail "SHA-256 mismatch for ${src}: $h (approved $($f.Sha256))" }
            $dst = Join-Path $tmp $f.Target
            Copy-Item -LiteralPath $src -Destination $dst
            if ((Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash -cne $f.Sha256) { Fail "copy hash mismatch for $dst" }
            Write-Host ("Source      : {0} ({1} B) SHA-256 {2} OK -> upload as {3}" -f $src, (Get-Item -LiteralPath $src).Length, $h, $f.Target)
            [pscustomobject]@{ Local = $dst; Name = $f.Target; Size = (Get-Item -LiteralPath $dst).Length }
        }
        if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
        Connect-Console $cfg 1800
        Write-Host "Root        : $(Confirm-DevFolder '\Vitox' 'firmware-staging')"
        Write-Host "Input dir   : $(Confirm-DevFolder $script:FirmwareRoot 'input')"
        $dir = "$($script:FirmwareRoot)\input"
        foreach ($s in $staged) {
            $t0 = Get-Date
            $r = Send-DevFile $dir $s.Local $s.Name
            Write-Host ("  upload {0}\{1} ({2} B) -> {3} in {4:N0} s" -f $dir, $s.Name, $s.Size, $(if ($r.DryRun) { 'dryrun' } else { "HTTP $($r.Status)" }), ((Get-Date) - $t0).TotalSeconds)
        }
        if ($script:DryRun) { return }
        $items = Get-DevFiles $dir
        $bad = 0
        foreach ($s in $staged) {
            $it = @($items | Where-Object { $_.Name -ceq $s.Name }) | Select-Object -First 1
            if (-not $it -or [int64]$it.FileSize -ne [int64]$s.Size) { $bad++; Write-Host "  VERIFY FAIL: $($s.Name)" -ForegroundColor Red }
        }
        Write-Host "Verify      : $($staged.Count - $bad)/$($staged.Count) uploaded with the same size (the on-console installer re-hashes each before use)"
        if ($bad) { Fail 'firmware-push verification failed' }
    } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------- M9A.2C app staging (DevelopmentFiles\Vitox\app-staging)

# PS Vita license (RIF) layout, the generic format only: 512 bytes, version 1 (big-endian) at 0x00, the 36-character
# content ID at 0x10, NUL-padded to 0x40. Nothing here is specific to any title.
$script:LicenseSize = 512
function Read-LicenseContentId([byte[]]$B) {
    if (-not $B -or $B.Length -ne $script:LicenseSize -or $B[0] -ne 0 -or $B[1] -ne 1) { return $null }
    for ($i = 0x34; $i -lt 0x40; $i++) { if ($B[$i] -ne 0) { return $null } }
    $cid = [Text.Encoding]::ASCII.GetString($B, 0x10, 36)
    if ($cid -cnotmatch $script:ContentIdPattern) { return $null }
    return $cid
}

# A license is staged only for the app it belongs to: exact size, a well-formed content ID equal to the app's
# param.sfo CONTENT_ID, and the TITLE_ID inside that content ID equal to the app's (and the destination's) TITLE_ID.
function Test-LicenseFile([byte[]]$B, $Sfo, [string]$Dest) {
    $r = [pscustomobject]@{ Ok = $false; Reason = $null; ContentId = $null; DerivedTitleId = $null }
    if (-not $B -or $B.Length -ne $script:LicenseSize) { $r.Reason = "license is $(if ($B) { $B.Length } else { 0 }) B, expected exactly $($script:LicenseSize) B"; return $r }
    $cid = Read-LicenseContentId $B
    if (-not $cid) { $r.Reason = 'no well-formed content ID in the license (version 1 header, content ID at 0x10, NUL padding to 0x40)'; return $r }
    $r.ContentId = $cid; $r.DerivedTitleId = $cid.Substring(7, 9)
    # M9A.3A: the key field (0x50..0x5F) must not be all zero (only tested for zero; the key is never used here)
    if (-not ($B[0x50..0x5F] | Where-Object { $_ -ne 0 })) { $r.Reason = 'license key field is all zero'; return $r }
    $appCid = ([string]$Sfo['CONTENT_ID']).Trim()
    $appId = ([string]$Sfo['TITLE_ID']).Trim()
    if (-not $appCid) { $r.Reason = 'the app param.sfo has no CONTENT_ID'; return $r }
    if ($cid -cne $appCid) { $r.Reason = "license content ID '$cid' differs from the app param.sfo CONTENT_ID '$appCid'"; return $r }
    if ($r.DerivedTitleId -cne $appId) { $r.Reason = "license TITLE_ID '$($r.DerivedTitleId)' differs from the app param.sfo TITLE_ID '$appId'"; return $r }
    if ($Dest -and $r.DerivedTitleId -cne $Dest) { $r.Reason = "license TITLE_ID '$($r.DerivedTitleId)' differs from the selected TITLE_ID '$Dest'"; return $r }
    $r.Ok = $true
    return $r
}

# license material never travels inside an app: it is staged separately (license-push)
function Get-LicenseLikeReason([string]$Rel, [string]$Full, [int64]$Size) {
    $l = @($Rel.Split('\') | ForEach-Object { $_.ToLowerInvariant() })
    if ($l.Count -ge 2 -and $l[0] -eq 'sce_sys' -and ($l[1] -eq 'package' -or $l[1] -eq 'packages')) { return 'sce_sys\package(s) holds license/package material' }
    if ($l[-1] -eq 'work.bin' -or $l[-1].EndsWith('.rif')) { return 'license file name' }
    if ($Size -eq $script:LicenseSize -and (Read-LicenseContentId ([IO.File]::ReadAllBytes($Full)))) { return 'license-format content (RIF header + content ID)' }
    return $null
}

# Local, read-only: validates an app folder for app-staging and returns the upload plan (fails closed on any problem)
function Get-AppPushPlan([string]$Source, [string]$Dest) {
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { Fail "-Source '$Source' is not a folder" }
    $src = (Resolve-Path -LiteralPath $Source).Path
    if (-not (Test-Path -LiteralPath (Join-Path $src 'eboot.bin') -PathType Leaf)) { Fail "$src has no eboot.bin" }
    $sfoPath = Join-Path $src 'sce_sys\param.sfo'
    if (-not (Test-Path -LiteralPath $sfoPath -PathType Leaf)) { Fail "$src has no sce_sys\param.sfo" }
    $sfo = Read-Sfo ([IO.File]::ReadAllBytes($sfoPath))
    $id = ([string]$sfo['TITLE_ID']).Trim()
    if ($id -cnotmatch $script:TitleIdPattern) { Fail "param.sfo TITLE_ID '$id' is not 9 characters A-Z/0-9" }
    if ($Dest -and $Dest -cne $id) { Fail "destination TITLE_ID '$Dest' differs from the source param.sfo TITLE_ID '$id'" }
    $all = @(Get-ChildItem -LiteralPath $src -Recurse -Force)
    $links = @($all | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
    if ($links.Count) { Fail "refusing: $($links.Count) symlink/junction item(s) in the source (first: $($links[0].FullName))" }
    $files = @($all | Where-Object { -not $_.PSIsContainer })
    $total = [int64]($files | Measure-Object Length -Sum).Sum
    if ($files.Count -gt 20000 -or $total -gt 4GB) { Fail "refusing: $($files.Count) files / $total B (limits 20000 / 4 GiB)" }
    $base = "$($script:AppStagingRoot)\$id"
    $plan = foreach ($f in $files) {
        $rel = [IO.Path]::GetRelativePath($src, $f.FullName)
        $segs = $rel.Split([IO.Path]::DirectorySeparatorChar)
        foreach ($s in $segs) { if (-not (Test-HomebrewName $s)) { Fail "refusing: '$rel' has a segment that is not a plain name" } }
        $lic = Get-LicenseLikeReason ($segs -join '\') $f.FullName $f.Length
        if ($lic) { Fail "refusing: '$rel' is license material ($lic) - stage licenses with license-push, never inside the app" }
        $dir = if ($segs.Count -gt 1) { $segs[0..($segs.Count - 2)] -join '\' } else { '' } # (0..-1 would wrap in PowerShell)
        $remoteDir = if ($dir) { "$base\$dir" } else { $base }
        if (-not (Test-AppStagingPath $remoteDir -ForFile)) { Fail "refusing: '$remoteDir' is outside the app-staging scope" }
        [pscustomobject]@{ Local = $f.FullName; Rel = ($segs -join '\'); Dir = $dir; RemoteDir = $remoteDir; Name = $segs[-1]; Size = $f.Length
            Sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash }
    }
    return [pscustomobject]@{ Source = $src; TitleId = $id; Title = $sfo['TITLE']; ContentId = ([string]$sfo['CONTENT_ID']).Trim(); Base = $base
        Files = @($plan); Total = $total; Sfo = $sfo }
}

# relative path -> remote item, every file below DevelopmentFiles<$P> (read-only)
function Get-RemoteTree([string]$P, [string]$Rel = '') {
    $t = @{}
    foreach ($it in @(Get-DevFiles $P | Where-Object { $_ })) { # (a missing folder is $null, not an empty list)
        $r = if ($Rel) { "$Rel\$($it.Name)" } else { $it.Name }
        if (Test-DevItemIsFolder $it) { $sub = Get-RemoteTree "$P\$($it.Name)" $r; foreach ($k in $sub.Keys) { $t[$k] = $sub[$k] } }
        else { $t[$r] = $it }
    }
    return $t
}
function Get-DevFileSha256([string]$P, [string]$Name) {
    $tmp = [IO.Path]::GetTempFileName()
    try {
        $r = Invoke-Wdp -Method GET -Path '/api/filesystem/apps/file' -Query ([ordered]@{ knownfolderid = 'DevelopmentFiles'; filename = $Name; path = $P }) -OutFile $tmp -AllowFailure
        if ($r.Status -ne 200) { return $null }
        return (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
}
# every planned file: listed with the same size AND downloaded back with the same SHA-256
function Confirm-ReadBack($Plan) {
    $bad = 0
    foreach ($p in $Plan) {
        $it = @(Get-DevFiles $p.RemoteDir | Where-Object { $_.Name -ceq $p.Name }) | Select-Object -First 1
        $sizeOk = $it -and [int64]$it.FileSize -eq [int64]$p.Size
        $sha = if ($sizeOk) { Get-DevFileSha256 $p.RemoteDir $p.Name } else { $null }
        if (-not $sizeOk -or $sha -cne $p.Sha256) { $bad++; Write-Host "  VERIFY FAIL: $($p.RemoteDir)\$($p.Name) (size $(if ($it) { $it.FileSize } else { 'absent' }) / $($p.Size), SHA-256 $(if ($sha) { $sha.Substring(0, 16) } else { '-' }))" -ForegroundColor Red }
    }
    return $bad
}

function Invoke-AppPush {
    $cfg = Get-HarnessConfig
    if (-not $Source) { Fail '-Source <local app folder> is required' }
    $plan = Get-AppPushPlan $Source $TitleId
    Write-Host "Source      : $($plan.Source) -> DevelopmentFiles$($plan.Base) | TITLE_ID=$($plan.TitleId) CONTENT_ID=$(if ($plan.ContentId) { $plan.ContentId } else { '-' }) | $($plan.Files.Count) file(s), $($plan.Total) B | license material: none"
    if ($script:DryRun) {
        foreach ($p in $plan.Files) { Write-Host ("  [DRYRUN] would upload {0}\{1} ({2} B, SHA-256 {3})" -f $p.RemoteDir, $p.Name, $p.Size, $p.Sha256.Substring(0, 16)) }
        Write-Host "[DRYRUN] app-push plan only: no console traffic, nothing uploaded"
        return
    }
    if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
    Connect-Console $cfg 1800
    # never delete, never overwrite: an existing app folder must hold only identical copies of source files
    $remote = Get-RemoteTree $plan.Base
    $byRel = @{}; foreach ($p in $plan.Files) { $byRel[$p.Rel] = $p }
    $extra = @($remote.Keys | Where-Object { -not $byRel.ContainsKey($_) })
    if ($extra.Count) { Fail "refusing: DevelopmentFiles$($plan.Base) has $($extra.Count) file(s) not in the source (first: $($extra[0])); nothing is deleted, so the staged app would not equal the source" }
    $todo = foreach ($p in $plan.Files) {
        if (-not $remote.ContainsKey($p.Rel)) { $p; continue }
        if ([int64]$remote[$p.Rel].FileSize -eq [int64]$p.Size -and (Get-DevFileSha256 $p.RemoteDir $p.Name) -ceq $p.Sha256) { continue }
        Fail "refusing: DevelopmentFiles$($p.RemoteDir)\$($p.Name) exists with different content (no overwrite)"
    }
    $todo = @($todo)
    Write-Host "Target      : $($remote.Count) file(s) already staged identically; $($todo.Count) to upload"
    Write-Host "Root        : $(Confirm-DevFolder '\Vitox' 'app-staging')"
    Write-Host "App folder  : $(Confirm-DevFolder $script:AppStagingRoot $plan.TitleId)"
    $made = @{}
    foreach ($d in @($plan.Files | ForEach-Object Dir | Where-Object { $_ } | Sort-Object -Unique)) {
        $acc = $plan.Base
        foreach ($s in $d.Split('\')) {
            if (-not $made.ContainsKey("$acc\$s")) { $null = Confirm-DevFolder $acc $s; $made["$acc\$s"] = $true }
            $acc = "$acc\$s"
        }
    }
    foreach ($p in $todo) {
        $r = Send-DevFile $p.RemoteDir $p.Local $p.Name
        Write-Host ("  upload {0}\{1} ({2} B, SHA-256 {3}) -> HTTP {4}" -f $p.RemoteDir, $p.Name, $p.Size, $p.Sha256.Substring(0, 16), $r.Status)
    }
    $bad = Confirm-ReadBack $plan.Files
    Write-Host "Verify      : $($plan.Files.Count - $bad)/$($plan.Files.Count) files read back with the same size and SHA-256"
    if ($bad) { Fail 'app-push verification failed' }
}

function Invoke-LicensePush {
    $cfg = Get-HarnessConfig
    if (-not $License) { Fail '-License <license file> is required' }
    if ($TitleId -cnotmatch $script:TitleIdPattern) { Fail "-TitleId <staged app TITLE_ID> is required (9 characters A-Z/0-9): '$TitleId'" }
    if (-not (Test-Path -LiteralPath $License -PathType Leaf)) { Fail "-License file not found: $License" }
    $lic = (Resolve-Path -LiteralPath $License).Path
    $bytes = [IO.File]::ReadAllBytes($lic)
    $sha = (Get-FileHash -LiteralPath $lic -Algorithm SHA256).Hash
    $checks = @()
    if ($Source) {
        $sfoPath = Join-Path $Source 'sce_sys\param.sfo'
        if (-not (Test-Path -LiteralPath $sfoPath -PathType Leaf)) { Fail "-Source has no sce_sys\param.sfo: $Source" }
        $checks += [pscustomobject]@{ What = "local app $Source"; Sfo = (Read-Sfo ([IO.File]::ReadAllBytes($sfoPath))) }
    }
    if ($script:DryRun -and -not $checks.Count) { Fail 'license-push -DryRun needs -Source <local app folder> (the dry run sends nothing, so the staged param.sfo is not read)' }
    if (-not $script:DryRun) {
        if (-not $cfg.PinnedCertSha256) { Fail 'console TLS certificate not pinned; run status + trust first' }
        Connect-Console $cfg 120
        $rs = Get-DevFileBytes "$($script:AppStagingRoot)\$TitleId\sce_sys" 'param.sfo'
        if (-not $rs) { Fail "refusing: no staged app at DevelopmentFiles$($script:AppStagingRoot)\$TitleId (app-push it first)" }
        $checks += [pscustomobject]@{ What = "staged app DevelopmentFiles$($script:AppStagingRoot)\$TitleId"; Sfo = (Read-Sfo $rs) }
    }
    $v = $null
    foreach ($c in $checks) {
        $v = Test-LicenseFile $bytes $c.Sfo $TitleId
        if (-not $v.Ok) { Fail "license-push refused against the $($c.What): $($v.Reason)" }
        Write-Host "License     : $lic ($($bytes.Length) B, SHA-256 $($sha.Substring(0, 16))...) content ID $($v.ContentId) -> TITLE_ID $($v.DerivedTitleId) matches the $($c.What)"
    }
    $dir = "$($script:AppStagingRoot)\$($script:LicenseFolder)\$($v.ContentId)"
    if (-not (Test-AppStagingPath $dir -ForFile)) { Fail "refusing: '$dir' is outside the app-staging scope" }
    if ($script:DryRun) { Write-Host "[DRYRUN] would upload $dir\work.bin ($($bytes.Length) B) - license-push plan only: no console traffic, nothing uploaded"; return }
    # one license per content ID: an identical copy is a no-op, anything else already there is refused (no overwrite)
    $existing = @(Get-DevFiles $dir | Where-Object { $_ })
    if ($existing.Count) {
        if ($existing.Count -eq 1 -and $existing[0].Name -ceq 'work.bin' -and (Get-DevFileSha256 $dir 'work.bin') -ceq $sha) { Write-Host "Target      : DevelopmentFiles$dir\work.bin already staged identically"; return }
        Fail "refusing: DevelopmentFiles$dir already holds $($existing.Count) item(s) that are not this license (no overwrite)"
    }
    Write-Host "Root        : $(Confirm-DevFolder '\Vitox' 'app-staging')"
    Write-Host "Licenses    : $(Confirm-DevFolder $script:AppStagingRoot $script:LicenseFolder)"
    Write-Host "Content ID  : $(Confirm-DevFolder "$($script:AppStagingRoot)\$($script:LicenseFolder)" $v.ContentId)"
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('vitox-lic-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $tmp
    try {
        $local = Join-Path $tmp 'work.bin'
        [IO.File]::WriteAllBytes($local, $bytes)
        $r = Send-DevFile $dir $local 'work.bin'
        Write-Host "  upload $dir\work.bin ($($bytes.Length) B) -> HTTP $($r.Status)"
    } finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
    $bad = Confirm-ReadBack @([pscustomobject]@{ RemoteDir = $dir; Name = 'work.bin'; Size = $bytes.Length; Sha256 = $sha })
    Write-Host "Verify      : license read back with the same size and SHA-256: $(if ($bad) { 'FAIL' } else { 'OK' })"
    if ($bad) { Fail 'license-push verification failed' }
}

# ---------------------------------------------------------------- selftest

function Invoke-Selftest {
    $results = [System.Collections.Generic.List[object]]::new()
    $check = {
        param([string]$Name, [scriptblock]$Body)
        try {
            $detail = & $Body
            $results.Add([pscustomobject]@{ Test = $Name; Result = 'PASS'; Detail = "$detail" })
        } catch {
            $results.Add([pscustomobject]@{ Test = $Name; Result = 'FAIL'; Detail = $_.Exception.Message })
        }
    }
    $assert = { param($cond, [string]$msg) if (-not $cond) { throw "assertion failed: $msg" } }
    $cfg = Get-HarnessConfig
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("vitox-harness-selftest-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    $null = New-Item -ItemType Directory $tmp
    $pkgPath = if ($Package) { $Package } else { $cfg.SelftestPackage }

    & $check 'config: loaded and valid' { "url=$($cfg.ConsoleUrl) auth=$($cfg.AuthEnabled) pinned=$([bool]$cfg.PinnedCertSha256)" }
    & $check 'config: plain HTTP rejected' {
        $c = $cfg | ConvertTo-Json -Depth 8 | ConvertFrom-Json; $c.ConsoleUrl = 'http://192.0.2.10:11080'
        & $assert ((Test-HarnessConfig $c).Count -gt 0) 'http accepted'; 'rejected' }
    & $check "config: 'auto-' username rejected" {
        $c = $cfg | ConvertTo-Json -Depth 8 | ConvertFrom-Json; $c.AuthEnabled = $true
        $c | Add-Member Username 'auto-admin' -Force; $c | Add-Member CredentialFile 'x.xml' -Force
        & $assert (@(Test-HarnessConfig $c) -match 'auto-').Count 'auto- accepted'; 'rejected' }
    & $check 'config: bad pinned thumbprint rejected' {
        $c = $cfg | ConvertTo-Json -Depth 8 | ConvertFrom-Json; $c | Add-Member PinnedCertSha256 'ABC' -Force
        & $assert ((Test-HarnessConfig $c).Count -gt 0) 'bad pin accepted'; 'rejected' }

    $info = $null
    & $check 'package: manifest extraction' {
        $script:stInfo = Get-PackageInfo $pkgPath; $i = $script:stInfo
        & $assert ($i.Name -ceq 'Vitox.M1Probe') "name $($i.Name)"
        & $assert ($i.Publisher -ceq 'CN=Vitox') "publisher $($i.Publisher)"
        & $assert ($i.Version -eq '0.0.1.0') "version $($i.Version)"
        & $assert ($i.Architecture -eq 'x64') "arch $($i.Architecture)"
        & $assert ($i.AppIds -contains 'App') 'AppId App missing'
        & $assert ($i.Capabilities -contains 'codeGeneration') 'codeGeneration missing'
        "$($i.PackageFullName) deps=$(@($i.Dependencies | ForEach-Object { "$($_.Name)>=$($_.MinVersion)" }) -join ',')" }
    $info = $script:stInfo
    & $check 'package: PFN publisher-id hash (known vectors)' {
        & $assert ([VitoxHarness.PackageId]::PublisherId('CN=Vitox') -eq '1baxdpdgcd2t6') 'CN=Vitox -> 1baxdpdgcd2t6 (observed via Get-AppxPackage)'
        & $assert ([VitoxHarness.PackageId]::PublisherId('CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US') -eq '8wekyb3d8bbwe') 'Microsoft -> 8wekyb3d8bbwe'
        & $assert ($info.PackageFamilyName -ceq $cfg.Expected.PackageFamilyName) "PFN $($info.PackageFamilyName)"
        $info.PackageFamilyName }
    & $check 'package: SHA-256 + size match release SHA256SUMS' {
        $h = Test-ReleaseHashes $pkgPath
        & $assert $h.Ok ($h.Mismatches -join '; ')
        $line = Get-Content (Join-Path (Split-Path $info.Path) 'SHA256SUMS.txt') | Where-Object { $_ -match [regex]::Escape($info.FileName) }
        & $assert ($line -match "^$($info.Sha256)\s+$($info.Size)\s") 'msix line mismatch'
        "$($info.Sha256) $($info.Size) B" }
    & $check 'package: identity matches config expectations' { $p = Test-ExpectedIdentity $info $cfg; & $assert ($p.Count -eq 0) ($p -join '; '); 'ok' }
    & $check 'symbols: PDB GUID matches exe-in-msix RSDS' { $s = Test-Symbols $info; & $assert $s.Match $s.Reason; "$($s.PdbGuid) age exe=$($info.ExeDebugId.Age) pdb=$($s.PdbAge)" }
    & $check 'symbols: mismatch detected' {
        $fake = $info | ConvertTo-Json -Depth 6 | ConvertFrom-Json; $fake.ExeDebugId.Guid = '00000000-0000-0000-0000-000000000000'
        & $assert (-not (Test-Symbols $fake).Match) 'fake GUID matched'; 'detected' }
    & $check 'symbols: missing PDB detected' {
        $fake = $info | ConvertTo-Json -Depth 6 | ConvertFrom-Json; $fake.Path = Join-Path $tmp 'x.msix'
        & $assert (-not (Test-Symbols $fake).Match) 'missing pdb matched'; 'detected' }

    & $check 'run dir: creation + run.json/summary.md skeleton' {
        $d = New-RunDir $tmp 'self test/label'
        foreach ($s in 'pre', 'post', 'logs', 'crash') { & $assert (Test-Path (Join-Path $d $s)) "missing $s" }
        Save-RunJson $d ([ordered]@{ label = 'selftest'; package = $info }); 'x' | Set-Content (Join-Path $d 'summary.md')
        & $assert ((Get-RunJson $d).package.Sha256 -eq $info.Sha256) 'run.json roundtrip'
        $script:stRun = $d; Split-Path $d -Leaf }

    & $check 'gate: no gate file -> refused' { & $assert (-not (Test-Gate $tmp 'confirmed').Ok) 'launched without gate'; 'refused' }
    & $check 'gate: wrong replies refused' {
        New-Gate $script:stRun
        foreach ($r in @($null, '', 'yes', 'confirm', 'confirmed!', 'not confirmed', 'ok confirmed', 'Please confirm')) {
            & $assert (-not (Test-Gate $script:stRun $r).Ok) "accepted '$r'" }
        'refused 8 variants' }
    & $check 'gate: exact phrase accepted (case-insensitive, trimmed)' {
        foreach ($r in 'confirmed', 'Confirmed', 'CONFIRMED', '  confirmed  ') { & $assert (Test-Gate $script:stRun $r).Ok "rejected '$r'" }
        'accepted 4 variants' }
    & $check 'gate: CONFIRMED state cannot be reused' {
        Set-GateConfirmed $script:stRun 'confirmed'
        & $assert ((Read-Json (Join-Path $script:stRun 'gate.json')).state -eq 'CONFIRMED') 'state not persisted'
        & $assert (-not (Test-Gate $script:stRun 'confirmed').Ok) 'reused confirmed gate'; 'refused' }

    $v = { param($a, $b, $c, $d) [pscustomobject]@{ Major = $a; Minor = $b; Build = $c; Revision = $d } }
    $mk = { param($ver, $pub = 'CN=Vitox', $pfn = 'Vitox.M1Probe_1baxdpdgcd2t6') $p = $ver.Split('.')
        [pscustomobject]@{ Name = 'Vitox.M1Probe'; Publisher = $pub; PackageFamilyName = $pfn; PackageFullName = "Vitox.M1Probe_$($ver)_x64__1baxdpdgcd2t6"; Version = (& $v $p[0] $p[1] $p[2] $p[3]) } }
    $ledgerSame = @([pscustomobject]@{ PackageFullName = $info.PackageFullName; Sha256 = $info.Sha256 })
    $ledgerDiff = @([pscustomobject]@{ PackageFullName = $info.PackageFullName; Sha256 = ('0' * 64) })
    $cases = @(
        @('absent -> install', @(), @(), 'install', $true),
        @('higher -> update', @(& $mk '0.0.0.9'), @(), 'update', $true),
        @('same ver + same hash (ledger) -> none', @(& $mk '0.0.1.0'), $ledgerSame, 'none', $true),
        @('same ver + different hash -> refuse', @(& $mk '0.0.1.0'), $ledgerDiff, 'refuse', $false),
        @('same ver + unknown content -> refuse', @(& $mk '0.0.1.0'), @(), 'refuse', $false),
        @('lower -> refuse', @(& $mk '0.0.2.0'), @(), 'refuse', $false),
        @('publisher mismatch -> refuse', @(& $mk '0.0.0.1' 'CN=Other'), @(), 'refuse', $false),
        @('PFN mismatch -> refuse', @(& $mk '0.0.0.1' 'CN=Vitox' 'Vitox.M1Probe_xxxxxxxxxxxxx'), @(), 'refuse', $false),
        @('two installed -> refuse (ambiguous)', @((& $mk '0.0.0.1'), (& $mk '0.0.0.2')), @(), 'refuse', $false)
    )
    foreach ($c in $cases) {
        & $check "policy: $($c[0])" {
            $d = Get-UpdateDecision $info $c[1] $c[2]
            & $assert ($d.Action -eq $c[3] -and $d.Allowed -eq $c[4]) "got $($d.Action)/$($d.Allowed): $($d.Reason)"; $d.Reason }
    }

    & $check 'http: destructive endpoints not allow-listed' {
        foreach ($e in @('DELETE /api/app/packagemanager/package', 'DELETE /api/filesystem/apps/file', 'POST /api/filesystem/apps/file',
                'PUT /ext/settings/TreatUWPAppsAsGames', 'POST /api/filesystem/apps/rename', 'DELETE /api/debug/dump/usermode/crashdump', 'POST /api/control/restart')) {
            $m, $p = $e.Split(' ', 2); & $assert (-not (Test-EndpointAllowed $m $p)) "$e allowed" }
        'blocked 7' }
    & $check 'm8a scope: uploads/folders only inside DevelopmentFiles\Vitox\homebrew' {
        $F = '/api/filesystem/apps/file'; $D = '/api/filesystem/apps/folder'
        $ok = @(
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew\VITOX0003\sce_sys' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox'; newfoldername = 'homebrew' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew'; newfoldername = 'VITOX0003' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox'; newfoldername = 'firmware-staging' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\firmware-staging'; newfoldername = 'input' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\firmware-staging\input' }))
        $bad = @(
            @('POST', $F, [ordered]@{ knownfolderid = 'LocalAppData'; path = '\Vitox\homebrew' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\vita-fs\ux0\app' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew\..\vita-fs' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrewX' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\firmware-stagingX' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\firmware-staging\..\vita-fs' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\vita-fs\vs0' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox'; newfoldername = 'firmware' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew\a\.\b' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\vitox\homebrew' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew'; extract = 'true' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew'; packagefullname = 'x' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles' }),
            @('POST', $F, $null),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox'; newfoldername = 'vita-fs' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\'; newfoldername = 'Vitox' }),
            @('POST', $D, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew'; newfoldername = '..' }),
            @('DELETE', $F, [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew' }),
            @('POST', '/api/filesystem/apps/rename', [ordered]@{ knownfolderid = 'DevelopmentFiles'; path = '\Vitox\homebrew' }))
        foreach ($c in $ok) { & $assert (Test-HomebrewScope $c[0] $c[1] $c[2]) "refused in-scope $($c[0]) $($c[1]) $($c[2] | ConvertTo-Json -Compress)" }
        foreach ($c in $bad) { & $assert (-not (Test-HomebrewScope $c[0] $c[1] $c[2])) "allowed out-of-scope $($c[0]) $($c[1]) $($c[2] | ConvertTo-Json -Compress)" }
        & $assert (-not (Test-EndpointAllowed 'POST' $F)) 'file upload became generally allow-listed'
        & $assert (-not (Test-HomebrewName '..') -and -not (Test-HomebrewName 'a/b') -and -not (Test-HomebrewName 'a\b') -and (Test-HomebrewName 'eboot.bin')) 'name rule'
        "in-scope $($ok.Count) allowed, out-of-scope $($bad.Count) refused" }
    & $check 'm8a sfo: TITLE_ID / TITLE parsed from a real param.sfo' {
        $s = Read-Sfo ([IO.File]::ReadAllBytes('C:\Vitox\Vitox-Homebrew\VITOX0003\out\VITOX0003\sce_sys\param.sfo'))
        & $assert ($s['TITLE_ID'] -ceq 'VITOX0003' -and $s['TITLE'] -ceq 'Vitox GXM Triangle') "got $($s['TITLE_ID'])/$($s['TITLE'])"
        "$($s['TITLE_ID']) '$($s['TITLE'])'" }
    & $check 'http: Invoke-Wdp refuses non-allow-listed call before any I/O' {
        $script:BaseUri = [uri]'https://192.0.2.1:11443'
        try { $null = Invoke-Wdp -Method DELETE -Path '/api/app/packagemanager/package' -Query ([ordered]@{ package = 'x' }); throw 'not refused' }
        catch { & $assert ($_.Exception.Message -match 'not allow-listed') $_.Exception.Message }
        'refused' }
    & $check 'dryrun: launch/stop/install are printed, not sent' {
        $saved = $script:DryRun; $script:DryRun = $true; $script:BaseUri = [uri]'https://192.0.2.1:11443'; $script:Http = $null
        try {
            $a = Invoke-Wdp -Method POST -Path '/api/taskmanager/app' -Query ([ordered]@{ appid = (ConvertTo-Hex64 'App'); package = (ConvertTo-Hex64 $info.PackageFullName) })
            $b = Invoke-Wdp -Method DELETE -Path '/api/taskmanager/app' -Query ([ordered]@{ package = (ConvertTo-Hex64 $info.PackageFullName) })
            $c = Invoke-Wdp -Method POST -Path '/api/app/packagemanager/package' -Query ([ordered]@{ package = $info.FileName })
        } finally { $script:DryRun = $saved }
        & $assert ($a.DryRun -and $b.DryRun -and $c.DryRun) 'not dry'
        & $assert ($a.Uri -match 'appid=QXBw&package=') "launch uri $($a.Uri)"
        $a.Uri }
    & $check 'hex64: Base64(UTF-8) encoding' {
        & $assert ((ConvertTo-Hex64 'App') -eq 'QXBw') 'App'
        & $assert ((ConvertTo-Hex64 'Vitox.M1Probe_0.0.1.0_x64__1baxdpdgcd2t6') -eq 'Vml0b3guTTFQcm9iZV8wLjAuMS4wX3g2NF9fMWJheGRwZGdjZDJ0Ng==') 'full name'
        'ok' }
    & $check 'multipart: body length and headers' {
        $mp = [VitoxHarness.MultipartFiles]::new([string[]]@($info.Path))
        $bytes = $mp.ReadAsByteArrayAsync().GetAwaiter().GetResult()
        & $assert ($bytes.Length -eq $mp.Headers.ContentLength) "len $($bytes.Length) != $($mp.Headers.ContentLength)"
        $head = [Text.Encoding]::ASCII.GetString($bytes, 0, 200)
        & $assert ($head -match "name=`"$([regex]::Escape($info.FileName))`"; filename=`"$([regex]::Escape($info.FileName))`"") 'content-disposition'
        & $assert ($mp.Headers.ContentType.MediaType -eq 'multipart/form-data') 'content type'
        "$($bytes.Length) B" }

    & $check 'tls: unpinned cert rejected, observed thumbprint recorded' {
        $rsa = [Security.Cryptography.RSA]::Create(2048)
        $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=selftest-console', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $script:stCert = $req.CreateSelfSigned((Get-Date).AddDays(-1), (Get-Date).AddDays(1))
        $script:stSha = $script:stCert.GetCertHashString([Security.Cryptography.HashAlgorithmName]::SHA256)
        [VitoxHarness.Tls]::Pinned = $null
        & $assert (-not [VitoxHarness.Tls]::Check($script:stCert, 'RemoteCertificateChainErrors')) 'accepted without pin'
        & $assert ([VitoxHarness.Tls]::LastSeenSha256 -eq $script:stSha) 'not recorded'
        $script:stSha.Substring(0, 16) + '...' }
    & $check 'tls: pinned cert accepted, other cert rejected' {
        [VitoxHarness.Tls]::Pinned = $script:stSha
        & $assert ([VitoxHarness.Tls]::Check($script:stCert, 'RemoteCertificateChainErrors')) 'pinned rejected'
        $rsa2 = [Security.Cryptography.RSA]::Create(2048)
        $other = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=selftest-console', $rsa2, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1).CreateSelfSigned((Get-Date).AddDays(-1), (Get-Date).AddDays(1))
        & $assert (-not [VitoxHarness.Tls]::Check($other, 'RemoteCertificateChainErrors')) 'changed cert accepted'
        [VitoxHarness.Tls]::Pinned = $null; 'ok' }
    & $check 'tls: pin persistence requires matching observed thumbprint' {
        $cfgCopy = Join-Path $tmp 'cfg.json'; $cc = Read-Json $Config; $cc.PinnedCertSha256 = $null; Write-Json $cc $cfgCopy
        $sd = Join-Path $tmp 'state'; $null = New-Item -ItemType Directory $sd
        Write-Json ([ordered]@{ sha256 = $script:stSha }) (Join-Path $sd 'observed-cert.json')
        $ps = (Get-Process -Id $PID).Path
        $bad = & $ps -NoProfile -File $PSCommandPath trust -Config $cfgCopy -Thumbprint ('A' * 64) 2>&1
        & $assert ((Read-Json $cfgCopy).PinnedCertSha256 -ne ('A' * 64)) 'pinned a non-observed thumbprint'
        & $assert ($LASTEXITCODE -ne 0) 'trust with wrong thumbprint exited 0'
        $null = & $ps -NoProfile -File $PSCommandPath trust -Config $cfgCopy -Thumbprint $script:stSha 2>&1
        & $assert ($LASTEXITCODE -eq 0) 'trust with observed thumbprint failed'
        & $assert ((Read-Json $cfgCopy).PinnedCertSha256 -eq $script:stSha) 'observed thumbprint not persisted'
        $other = 'B' * 64; Write-Json ([ordered]@{ sha256 = $other }) (Join-Path $sd 'observed-cert.json')
        $null = & $ps -NoProfile -File $PSCommandPath trust -Config $cfgCopy -Thumbprint $other 2>&1
        & $assert ($LASTEXITCODE -ne 0 -and (Read-Json $cfgCopy).PinnedCertSha256 -eq $script:stSha) 'changed cert re-pinned without -ReplacePin'
        'wrong refused; observed pinned; change without -ReplacePin refused' }

    # ---- pin rotation (v0.1.4): status records a mismatch, trust -ReplacePin replaces explicitly
    $pinDir = Join-Path $tmp 'pin'; $null = New-Item -ItemType Directory $pinDir
    $pinCfg = Join-Path $pinDir 'cfg.json'; $pinObs = Join-Path $pinDir 'state\observed-cert.json'
    $oldPin = 'A' * 64
    $ps = (Get-Process -Id $PID).Path
    $runTrust = { param([string]$T, [switch]$Replace)
        $a = @('-NoProfile', '-File', $PSCommandPath, 'trust', '-Config', $pinCfg, '-Thumbprint', $T); if ($Replace) { $a += '-ReplacePin' }
        $o = & $ps @a 2>&1; [pscustomobject]@{ Exit = $LASTEXITCODE; Out = ($o | Out-String) } }
    & $check 'tls: status records pin mismatch, keeps old pin, sends no data' {
        $rsa = [Security.Cryptography.RSA]::Create(2048)
        $req = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=127.0.0.1', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
        $eph = $req.CreateSelfSigned((Get-Date).AddDays(-1), (Get-Date).AddDays(1))
        $srvCert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($eph.Export('Pfx'), [string]$null, 'UserKeySet')
        $script:stRotSha = $srvCert.GetCertHashString([Security.Cryptography.HashAlgorithmName]::SHA256)
        $srv = [VitoxHarness.SelftestTlsServer]::new($srvCert)
        try {
            $cc = Read-Json $Config; $cc.ConsoleUrl = "https://127.0.0.1:$($srv.Port)"; $cc.PinnedCertSha256 = $oldPin; Write-Json $cc $pinCfg
            $o = & $ps -NoProfile -File $PSCommandPath status -Config $pinCfg 2>&1
            & $assert ($LASTEXITCODE -eq $script:ExitPinMismatch) "status exit $LASTEXITCODE (want $($script:ExitPinMismatch)): $($o | Out-String)"
            Start-Sleep -Milliseconds 300
            & $assert ($srv.Connections -ge 1) 'status never connected'
            # Under TLS 1.3 the server side can complete before the client rejects the certificate, so the
            # requirement is on application data: the client must abort before sending any HTTP request.
            & $assert ($srv.AppBytesReceived -eq 0) "application data sent to a non-pinned cert: $($srv.AppBytesReceived) B"
            $script:stRotHs = $srv.Handshakes
        } finally { $srv.Dispose() }
        $ob = Read-Json $pinObs
        & $assert ($ob.kind -eq 'pin-mismatch' -and $ob.sha256 -eq $script:stRotSha -and $ob.pinned_at_observation -eq $oldPin) "observation kind=$($ob.kind) sha=$($ob.sha256) pin=$($ob.pinned_at_observation)"
        & $assert ((Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) 'old pin changed by status'
        "exit 3; 0 app bytes (server-side handshakes=$($script:stRotHs)); observed $($script:stRotSha.Substring(0, 16))...; old pin kept" }
    & $check 'tls: ReplacePin refuses an unobserved thumbprint' {
        $r = & $runTrust ('D' * 64) -Replace
        & $assert ($r.Exit -ne 0) 'unobserved thumbprint accepted'
        & $assert ((Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) 'old pin changed'
        'refused; old pin kept' }
    & $check 'tls: replacement refused without -ReplacePin, for a stale observation, or to the current pin' {
        $r = & $runTrust $script:stRotSha
        & $assert ($r.Exit -ne 0 -and (Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) 'replaced without -ReplacePin'
        $saved = Get-Content -Raw $pinObs
        $stale = Read-Json $pinObs; $stale.pinned_at_observation = 'F' * 64; Write-Json $stale $pinObs
        $r = & $runTrust $script:stRotSha -Replace
        & $assert ($r.Exit -ne 0 -and (Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) 'replaced from a mismatch recorded against another pin'
        $unp = Read-Json $pinObs; $unp.kind = 'unpinned'; $unp.pinned_at_observation = $oldPin; Write-Json $unp $pinObs
        $r = & $runTrust $script:stRotSha -Replace
        & $assert ($r.Exit -ne 0 -and (Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) "replaced from a non-mismatch observation"
        Set-Content -Path $pinObs -Value $saved -NoNewline
        $cc = Read-Json $pinCfg; $cc.PinnedCertSha256 = $script:stRotSha; Write-Json $cc $pinCfg
        $r = & $runTrust $script:stRotSha -Replace
        & $assert ($r.Exit -ne 0) 'replacing the current pin with itself accepted'
        $cc.PinnedCertSha256 = $oldPin; Write-Json $cc $pinCfg
        'refused 4 variants; old pin kept' }
    & $check 'tls: explicit ReplacePin to the observed thumbprint succeeds; old pin dropped only then' {
        & $assert ((Read-Json $pinCfg).PinnedCertSha256 -eq $oldPin) 'precondition: old pin not in place'
        $r = & $runTrust $script:stRotSha -Replace
        & $assert ($r.Exit -eq 0) "replace failed: $($r.Out)"
        $after = Read-Json $pinCfg
        & $assert ($after.PinnedCertSha256 -eq $script:stRotSha) "pin is $($after.PinnedCertSha256)"
        & $assert ($after.PreviousPinnedCertSha256 -eq $oldPin -and $after.PinReplacedAt) 'previous pin not recorded'
        & $assert ((Read-Json "$pinCfg.prev").PinnedCertSha256 -eq $oldPin) '.prev does not hold the old pin'
        & $assert (-not (Test-Path "$pinCfg.tmp")) 'temp config left behind'
        $r = & $runTrust $script:stRotSha -Replace
        & $assert ($r.Exit -ne 0) 'second replacement to the same thumbprint accepted'
        'replaced; previous recorded; .prev = old pin' }
    & $check 'hygiene: only Vitox dump/WER/package metadata is kept' {
        $dj = [pscustomobject]@{ CrashDumps = @(
                [pscustomobject]@{ FileName = 'Other.App.exe.1.dmp'; PackageFullName = 'Contoso.Other_1.0.0.0_x64__abcdefghijklm' },
                [pscustomobject]@{ FileName = 'VitoxM1Probe.exe.2764.dmp'; PackageFullName = 'Vitox.M1Probe_0.0.2.0_x64__1baxdpdgcd2t6' },
                [pscustomobject]@{ FileName = 'VitoxM1Probe.exe.99.dmp'; PackageFullName = '' }) }
        $d = Select-VitoxDumps $dj $cfg 'VitoxM1Probe.exe'
        & $assert (@($d.CrashDumps).Count -eq 2 -and $d.ignored_other_packages -eq 1) "dumps kept=$(@($d.CrashDumps).Count) ignored=$($d.ignored_other_packages)"
        & $assert (-not (($d | ConvertTo-Json -Depth 6) -match 'Contoso')) 'other package leaked into dumps'
        $wj = [pscustomobject]@{ WerReports = @(
                [pscustomobject]@{ User = 'Default'; Reports = @([pscustomobject]@{ Name = 'AppCrash_Other.App.exe_1'; Type = 'Queue' },
                        [pscustomobject]@{ Name = 'AppCrash_VitoxM1Probe.exe_ab12'; Type = 'Queue' }) },
                [pscustomobject]@{ User = 'SYSTEM'; Reports = @() }) }
        $w = Select-VitoxWer $wj $cfg 'VitoxM1Probe.exe'
        & $assert ($w.ignored_other_packages -eq 1 -and @($w.WerReports[0].Reports).Count -eq 1) "wer ignored=$($w.ignored_other_packages)"
        & $assert (-not (($w | ConvertTo-Json -Depth 6) -match 'Other\.App')) 'other package leaked into WER'
        $pj = [pscustomobject]@{ InstalledPackages = @(
                [pscustomobject]@{ Name = 'Contoso Thing'; PackageFamilyName = 'Contoso.Thing_abcdefghijklm'; PackageFullName = 'Contoso.Thing_1.0.0.0_x64__abcdefghijklm' },
                [pscustomobject]@{ Name = 'Vitox M2 Probe'; PackageFamilyName = 'Vitox.M1Probe'; PackageFullName = 'Vitox.M1Probe_0.0.2.0_x64__1baxdpdgcd2t6' },
                [pscustomobject]@{ Name = 'VCLibs'; PackageFullName = 'Microsoft.VCLibs.140.00_14.0.33519.0_x64__8wekyb3d8bbwe' }) }
        $p = Select-VitoxPackages $pj $cfg
        & $assert (@($p.InstalledPackages).Count -eq 2 -and $p.total_installed -eq 3) "packages kept=$(@($p.InstalledPackages).Count)"
        & $assert (-not (($p | ConvertTo-Json -Depth 6) -match 'Contoso')) 'other package leaked into packages'
        & $assert (@((Select-VitoxDumps $null $cfg 'VitoxM1Probe.exe').CrashDumps).Count -eq 0) 'null dumps json'
        'dumps 2/3, WER 1/2, packages 2/3; no foreign names written' }
    & $check 'launch target: single entry required unless -AllowStaleUserRegistration' {
        $ve = { param($a, $b, $c, $d) [pscustomobject]@{ Major = $a; Minor = $b; Build = $c; Revision = $d } }
        $ent = { param($ver, $pfn = 'Vitox.M1Probe_1baxdpdgcd2t6', $pub = 'CN=Vitox') $p = $ver.Split('.')
            [pscustomobject]@{ PackageFullName = "Vitox.M1Probe_$($ver)_x64__1baxdpdgcd2t6"; PackageFamilyName = $pfn; Publisher = $pub; Version = (& $ve $p[0] $p[1] $p[2] $p[3]) } }
        $rp = [pscustomobject]@{ PackageFullName = 'Vitox.M1Probe_0.0.3.0_x64__1baxdpdgcd2t6'; PackageFamilyName = 'Vitox.M1Probe_1baxdpdgcd2t6'; Publisher = 'CN=Vitox'; Version = '0.0.3.0' }
        & $assert (Select-LaunchTarget @(& $ent '0.0.3.0') $rp $false).Ok 'single entry refused'
        & $assert (-not (Select-LaunchTarget @((& $ent '0.0.2.0'), (& $ent '0.0.3.0')) $rp $false).Ok) 'two entries accepted without flag'
        $s = Select-LaunchTarget @((& $ent '0.0.2.0'), (& $ent '0.0.3.0')) $rp $true
        & $assert ($s.Ok -and @($s.Stale).Count -eq 1 -and $s.Entry.PackageFullName -ceq $rp.PackageFullName) 'stale lower version not tolerated with flag'
        & $assert (-not (Select-LaunchTarget @((& $ent '0.0.4.0'), (& $ent '0.0.3.0')) $rp $true).Ok) 'higher other version tolerated'
        & $assert (-not (Select-LaunchTarget @((& $ent '0.0.2.0' 'Other_x'), (& $ent '0.0.3.0')) $rp $true).Ok) 'other identity tolerated'
        & $assert (-not (Select-LaunchTarget @(& $ent '0.0.2.0') $rp $true).Ok) 'run package absent accepted'
        'single ok; 2 w/o flag refused; stale lower ok w/ flag; higher/other identity/absent refused' }
    & $check 'prepare: stale per-user registration reduction (0.1.8)' {
        $ve = { param($a, $b, $c, $d) [pscustomobject]@{ Major = $a; Minor = $b; Build = $c; Revision = $d } }
        $ent = { param($ver, $pfn = 'Vitox.M1Probe_1baxdpdgcd2t6', $pub = 'CN=Vitox') $p = $ver.Split('.')
            [pscustomobject]@{ Name = 'Vitox.M1Probe'; PackageFullName = "Vitox.M1Probe_$($ver)_x64__1baxdpdgcd2t6"; PackageFamilyName = $pfn; Publisher = $pub; Version = (& $ve $p[0] $p[1] $p[2] $p[3]) } }
        $in = [pscustomobject]@{ Name = 'Vitox.M1Probe'; Publisher = 'CN=Vitox'; PackageFamilyName = 'Vitox.M1Probe_1baxdpdgcd2t6'; PackageFullName = 'Vitox.M1Probe_0.0.5.0_x64__1baxdpdgcd2t6'; Version = '0.0.5.0'; Sha256 = ('A' * 64) }
        $two = @((& $ent '0.0.3.0'), (& $ent '0.0.4.0'))
        $r = Reduce-StaleForDecision $in $two $false
        & $assert (-not $r.Reduced -and (Get-UpdateDecision $in $r.Installed @()).Action -eq 'refuse') 'two entries reduced without flag'
        $r = Reduce-StaleForDecision $in $two $true
        & $assert ($r.Reduced -and @($r.Stale).Count -eq 1 -and $r.Stale[0] -like '*0.0.3.0*') 'stale lower not dropped with flag'
        & $assert ((Get-UpdateDecision $in $r.Installed @()).Action -eq 'update') 'reduced decision is not update'
        $r = Reduce-StaleForDecision $in @((& $ent '0.0.3.0' 'Other_x'), (& $ent '0.0.4.0')) $true
        & $assert (-not $r.Reduced -and (Get-UpdateDecision $in $r.Installed @()).Action -eq 'refuse') 'other PFN reduced'
        $r = Reduce-StaleForDecision $in @((& $ent '0.0.3.0' 'Vitox.M1Probe_1baxdpdgcd2t6' 'CN=Other'), (& $ent '0.0.4.0')) $true
        & $assert (-not $r.Reduced) 'other publisher reduced'
        $r = Reduce-StaleForDecision $in @((& $ent '0.0.4.0'), (& $ent '0.0.4.0')) $true
        & $assert (-not $r.Reduced) 'tied highest reduced'
        $r = Reduce-StaleForDecision $in @((& $ent '0.0.3.0'), (& $ent '0.0.6.0')) $true
        & $assert ($r.Reduced -and (Get-UpdateDecision $in $r.Installed @()).Action -eq 'refuse') 'highest above incoming not refused as downgrade'
        'no flag: refuse; flag: lower dropped -> update; other PFN/publisher/tie: untouched; downgrade still refused' }
    & $check 'prepare: running-instance lifecycle (0.1.9)' {
        $inst = @([pscustomobject]@{ Name = 'Vitox.M1Probe'; Publisher = 'CN=Vitox'; PackageFullName = 'Vitox.M1Probe_0.0.13.3_x64__1baxdpdgcd2t6' },
            [pscustomobject]@{ Name = 'Other.App'; Publisher = 'CN=Other'; PackageFullName = 'Other.App_1.0.0.0_x64__abc' })
        $none = [pscustomobject]@{ Processes = @([pscustomobject]@{ ImageName = 'explorer.exe'; ProcessId = 1 }, [pscustomobject]@{ ImageName = 'VitoxM1ProbeHelper.exe'; ProcessId = 2 }) }
        $one = [pscustomobject]@{ Processes = @([pscustomobject]@{ ImageName = 'explorer.exe'; ProcessId = 1 }, [pscustomobject]@{ ImageName = 'VitoxM1Probe.exe'; ProcessId = 7100 }) }
        $p = Get-RunningVitoxPlan $none $inst $cfg $false
        & $assert ($p.Action -eq 'none' -and $p.Found -eq 0 -and -not $p.Targets.Count) 'no instance: not none (or unrelated/look-alike process matched)'
        $p = Get-RunningVitoxPlan $one $inst $cfg $false
        & $assert ($p.Action -eq 'stop' -and $p.Found -eq 1 -and $p.Pids[0] -eq 7100) 'one instance: not stop'
        & $assert (@($p.Targets).Count -eq 1 -and $p.Targets[0] -ceq 'Vitox.M1Probe_0.0.13.3_x64__1baxdpdgcd2t6') "stop targets $($p.Targets -join ',') (must be Vitox only)"
        $p = Get-RunningVitoxPlan $one $inst $cfg $true
        & $assert ($p.Action -eq 'keep' -and $p.Found -eq 1 -and -not $p.Targets.Count) 'keep-running override: not keep'
        $p = Get-RunningVitoxPlan ([pscustomobject]@{ Processes = $null }) $inst $cfg $false
        & $assert ($p.Action -eq 'none') 'empty process list'
        'none -> none; one Vitox -> stop (Vitox package only, unrelated untouched); -KeepRunningDiagnostic -> keep' }
    & $check 'logs: session marker matches M1, M2 and M2.5 probes' {
        & $assert ('2026 [1] ===== Vitox M1 Probe session start pid=10 =====' -match $script:SessionMarker) 'M1'
        & $assert ('2026 [1] ===== Vitox M2 Probe session start pid=11 =====' -match $script:SessionMarker -and $Matches[1] -eq '11') 'M2'
        & $assert ('2026 [1] ===== Vitox M2.5 Probe session start pid=12 =====' -match $script:SessionMarker -and $Matches[1] -eq '12') 'M2.5'
        & $assert (-not ('===== Vitox M2. Probe session start pid=1 =====' -match $script:SessionMarker)) 'malformed milestone matched'
        & $assert (-not ('===== Vitox Probe session start pid=1 =====' -match $script:SessionMarker)) 'bad marker matched'
        'ok' }
    & $check 'xbox: installed-package normalization (observed WDP shape)' {
        $obs = [pscustomobject]@{ Name = 'Vitox M1 Probe'; PackageFamilyName = 'Vitox.M1Probe'; PackageFullName = 'Vitox.M1Probe_0.0.1.0_x64__1baxdpdgcd2t6'
            PackageRelativeId = 'Vitox.M1Probe_1baxdpdgcd2t6!App'; Publisher = 'CN=Vitox'; Version = [pscustomobject]@{ Major = 0; Minor = 0; Build = 1; Revision = 0 } }
        $n = ConvertTo-InstalledIdentity $obs
        & $assert ($n.Valid -and $n.Name -ceq 'Vitox.M1Probe' -and $n.PackageFamilyName -ceq 'Vitox.M1Probe_1baxdpdgcd2t6') "normalized $($n.Name)/$($n.PackageFamilyName)"
        & $assert ((Get-UpdateDecision $info @($n) $ledgerSame).Action -eq 'none') 'same-hash decision on normalized entry'
        & $assert ((Get-UpdateDecision $info @($n) @()).Action -eq 'refuse') 'unknown-content decision on normalized entry'
        & $assert (-not (ConvertTo-InstalledIdentity ([pscustomobject]@{ PackageFullName = 'bogus' })).Valid) 'bogus full name accepted'
        "$($n.PackageFamilyName) (display '$($n.DisplayName)')" }
    & $check 'logs: session slicing by marker' {
        $lines = @('junk', '2026 [1] ===== Vitox M1 Probe session start pid=10 =====', 'a', '2026 [2] ===== Vitox M1 Probe session start pid=22 =====', 'b', 'c')
        $s = Split-Sessions $lines
        & $assert ($s.Count -eq 2 -and $s[0].Pid -eq 10 -and $s[1].Pid -eq 22 -and $s[1].Lines.Count -eq 3) 'bad split'
        '2 sessions' }
    & $check 'logs: new session despite PID reuse (0.1.7)' {
        $pre = Split-Sessions @('2026 [1] ===== Vitox M1 Probe session start pid=6500 =====', 'old', '2026 [2] ===== Vitox M1 Probe session start pid=7 =====', 'x')
        $post = Split-Sessions @('2026 [1] ===== Vitox M1 Probe session start pid=6500 =====', 'old', '2026 [2] ===== Vitox M1 Probe session start pid=7 =====', 'x',
            '2026 [3] ===== Vitox M4 Probe session start pid=6500 =====', 'new')
        $n = @(Select-NewSessions $post $pre.Count)
        & $assert ($n.Count -eq 1 -and $n[0].Pid -eq 6500 -and $n[0].Lines[-1] -eq 'new') 'reused PID not detected as new'
        & $assert (@(Select-NewSessions $pre $pre.Count).Count -eq 0) 'no new session expected'
        'reused pid 6500 -> 1 new session' }
    & $check 'logs: analysis parser on PC M1 logs' {
        $pcLogs = Join-Path $env:LOCALAPPDATA 'Packages\Vitox.M1Probe_1baxdpdgcd2t6\LocalState'
        if (-not (Test-Path (Join-Path $pcLogs 'lifecycle_log.txt'))) { return 'SKIP: no PC logs' }
        $d = Join-Path $tmp 'pclogs'; $null = New-Item -ItemType Directory $d
        foreach ($f in $cfg.LogFiles) {
            $p = Join-Path $pcLogs $f; if (-not (Test-Path $p)) { continue }
            $s = Split-Sessions (Get-Content $p); if ($s.Count) { $s[-1].Lines | Set-Content (Join-Path $d $f) }
        }
        $a = Get-SessionAnalysis $d
        & $assert ($a.device_family -eq 'Windows.Desktop') "family $($a.device_family)"
        & $assert (@($a.jit) -contains 'JIT: PASS') 'JIT PASS not parsed'
        & $assert ($a.mem_samples -gt 0) 'no mem samples'
        "jit=PASS api='$($a.api_summary)' mem_samples=$($a.mem_samples)" }
    # ---- M9A.2C app staging (synthetic IDs only: XX0000-/ABCD12345 are not real titles)
    $mkSfo = { param([System.Collections.Specialized.OrderedDictionary]$Kv)
        # minimal PSF writer (utf8 strings), the same layout Read-Sfo parses
        $keys = [IO.MemoryStream]::new(); $data = [IO.MemoryStream]::new(); $ents = [IO.MemoryStream]::new(); $bw = [IO.BinaryWriter]::new($ents)
        foreach ($k in $Kv.Keys) {
            $kb = [Text.Encoding]::ASCII.GetBytes($k + "`0"); $db = [Text.Encoding]::UTF8.GetBytes([string]$Kv[$k] + "`0")
            $max = [int]([Math]::Ceiling($db.Length / 4.0) * 4)
            $bw.Write([uint16]$keys.Length); $bw.Write([uint16]0x0204); $bw.Write([uint32]$db.Length); $bw.Write([uint32]$max); $bw.Write([uint32]$data.Length)
            $keys.Write($kb, 0, $kb.Length); $data.Write($db, 0, $db.Length); $data.Write((New-Object byte[] ($max - $db.Length)), 0, $max - $db.Length)
        }
        $bw.Flush(); $kt = 20 + $ents.Length; $dt = $kt + $keys.Length
        $o = [IO.MemoryStream]::new(); $w = [IO.BinaryWriter]::new($o)
        $w.Write([byte[]](0, 0x50, 0x53, 0x46)); $w.Write([uint32]0x101); $w.Write([uint32]$kt); $w.Write([uint32]$dt); $w.Write([uint32]$Kv.Count)
        $w.Write($ents.ToArray()); $w.Write($keys.ToArray()); $w.Write($data.ToArray()); $w.Flush(); , $o.ToArray() }
    $mkLic = { param([string]$Cid, [int]$Size = 512, [int]$Ver = 1, [switch]$ZeroKey)
        $b = New-Object byte[] $Size
        if ($Size -ge 0x40) { $b[1] = [byte]$Ver; $c = [Text.Encoding]::ASCII.GetBytes($Cid); [Array]::Copy($c, 0, $b, 0x10, [Math]::Min($c.Length, 0x30)) }
        if ($Size -ge 0x60 -and -not $ZeroKey) { $b[0x5F] = 0x5A } # synthetic non-zero key byte
        , $b }
    $stId = 'ABCD12345'; $stCid = 'XX0000-ABCD12345_00-0123456789ABCDEF'
    $mkApp = { param([string]$Name, [string]$Id = $stId, [string]$Cid = $stCid)
        $d = Join-Path $tmp "app-$Name"; $null = New-Item -ItemType Directory -Force (Join-Path $d 'sce_sys'), (Join-Path $d 'sce_module')
        [IO.File]::WriteAllBytes((Join-Path $d 'sce_sys\param.sfo'), (& $mkSfo ([ordered]@{ CONTENT_ID = $Cid; TITLE = 'Selftest App'; TITLE_ID = $Id })))
        [IO.File]::WriteAllBytes((Join-Path $d 'eboot.bin'), [byte[]](0x7F, 0x45, 0x4C, 0x46, 1, 2, 3))
        [IO.File]::WriteAllText((Join-Path $d 'sce_module\libc.suprx'), 'x')
        $d }
    & $check 'm9a.2c scope: app-staging writes only as <TITLE_ID>\... or licenses\<CONTENT_ID>' {
        $F = '/api/filesystem/apps/file'; $D = '/api/filesystem/apps/folder'; $dv = 'DevelopmentFiles'; $R = '\Vitox\app-staging'
        $ok = @(
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = '\Vitox'; newfoldername = 'app-staging' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = $R; newfoldername = $stId }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = "$R\$stId"; newfoldername = 'sce_sys' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = $R; newfoldername = 'licenses' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = "$R\licenses"; newfoldername = $stCid }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId\sce_sys\livearea" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\licenses\$stCid" }))
        $bad = @(
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = $R }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\licenses" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\licenses\$stCid\sub" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\licenses\XX0000-ABCD12345_00-0123456789abcdef" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\licenses\$stId" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\abcd12345" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\ABCD1234" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\Selftest" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId\..\..\homebrew" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId\a b" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = '\Vitox\app-stagingX\ABCD12345' }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = '\vitox\app-staging\ABCD12345' }),
            @('POST', $F, [ordered]@{ knownfolderid = 'LocalAppData'; path = "$R\$stId" }),
            @('POST', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId"; extract = 'true' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = $R; newfoldername = 'Selftest' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = $R; newfoldername = '..' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = "$R\licenses"; newfoldername = $stId }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = "$R\licenses\$stCid"; newfoldername = 'x' }),
            @('POST', $D, [ordered]@{ knownfolderid = $dv; path = "$R\$stId"; newfoldername = 'a\b' }),
            @('DELETE', $F, [ordered]@{ knownfolderid = $dv; path = "$R\$stId" }),
            @('POST', '/api/filesystem/apps/rename', [ordered]@{ knownfolderid = $dv; path = "$R\$stId" }))
        foreach ($c in $ok) { & $assert (Test-HomebrewScope $c[0] $c[1] $c[2]) "refused in-scope $($c[0]) $($c[1]) $($c[2] | ConvertTo-Json -Compress)" }
        foreach ($c in $bad) { & $assert (-not (Test-HomebrewScope $c[0] $c[1] $c[2])) "allowed out-of-scope $($c[0]) $($c[1]) $($c[2] | ConvertTo-Json -Compress)" }
        & $assert (-not (Test-HomebrewPath "$R\$stId")) 'app-staging became a generic homebrew-style write root'
        "in-scope $($ok.Count) allowed, out-of-scope $($bad.Count) refused (no delete/rename endpoint)" }
    & $check 'm9a.2c license: size, content ID, CONTENT_ID and TITLE_ID must all match (fails closed)' {
        $sfo = Read-Sfo (& $mkSfo ([ordered]@{ CONTENT_ID = $stCid; TITLE_ID = $stId }))
        $v = Test-LicenseFile (& $mkLic $stCid) $sfo $stId
        & $assert ($v.Ok -and $v.ContentId -ceq $stCid -and $v.DerivedTitleId -ceq $stId) "valid license refused: $($v.Reason)"
        $cases = @(
            @('511 B', (& $mkLic $stCid 511), $sfo, $stId),
            @('513 B', (& $mkLic $stCid 513), $sfo, $stId),
            @('empty', [byte[]]@(), $sfo, $stId),
            @('version 2 header', (& $mkLic $stCid 512 2), $sfo, $stId),
            @('all-zero key field', (& $mkLic $stCid -ZeroKey), $sfo, $stId),
            @('lower-case content ID', (& $mkLic $stCid.ToLowerInvariant()), $sfo, $stId),
            @('truncated content ID', (& $mkLic $stCid.Substring(0, 35)), $sfo, $stId),
            @('junk after content ID', (& $mkLic ($stCid + 'Z')), $sfo, $stId),
            @('other content ID, same title', (& $mkLic 'XX0000-ABCD12345_00-FEDCBA9876543210'), $sfo, $stId),
            @('other title', (& $mkLic 'XX0000-WXYZ54321_00-0123456789ABCDEF'), $sfo, $stId),
            @('app without CONTENT_ID', (& $mkLic $stCid), (Read-Sfo (& $mkSfo ([ordered]@{ TITLE_ID = $stId }))), $stId),
            @('sfo TITLE_ID differs from its CONTENT_ID', (& $mkLic $stCid), (Read-Sfo (& $mkSfo ([ordered]@{ CONTENT_ID = $stCid; TITLE_ID = 'WXYZ54321' }))), 'WXYZ54321'),
            @('selected TITLE_ID differs', (& $mkLic $stCid), $sfo, 'WXYZ54321'))
        foreach ($c in $cases) { $r = Test-LicenseFile $c[1] $c[2] $c[3]; & $assert (-not $r.Ok -and $r.Reason) "accepted: $($c[0])" }
        "valid accepted; $($cases.Count) mismatches refused" }
    & $check 'm9a.2c app-push plan: TITLE_ID match, no license material, plain names (local, read-only)' {
        $p = Get-AppPushPlan (& $mkApp 'good') $stId
        & $assert ($p.TitleId -ceq $stId -and $p.Base -ceq "\Vitox\app-staging\$stId" -and $p.Files.Count -eq 3 -and $p.ContentId -ceq $stCid) "plan $($p.TitleId) $($p.Base) $($p.Files.Count)"
        & $assert (@($p.Files | Where-Object { -not (Test-AppStagingPath $_.RemoteDir -ForFile) }).Count -eq 0) 'plan leaves the app folder'
        & $assert ((Get-AppPushPlan (& $mkApp 'nodest') '').TitleId -ceq $stId) 'default destination is not the param.sfo TITLE_ID'
        $refuse = [ordered]@{
            'destination TITLE_ID differs' = { Get-AppPushPlan (& $mkApp 'mism') 'WXYZ54321' }
            'bad param.sfo TITLE_ID' = { Get-AppPushPlan (& $mkApp 'badid' 'abc') '' }
            'no eboot.bin' = { $d = & $mkApp 'noeboot'; Remove-Item (Join-Path $d 'eboot.bin'); Get-AppPushPlan $d '' }
            'sce_sys\package' = { $d = & $mkApp 'pkg'; $null = New-Item -ItemType Directory (Join-Path $d 'sce_sys\package'); [IO.File]::WriteAllText((Join-Path $d 'sce_sys\package\head.bin'), 'x'); Get-AppPushPlan $d '' }
            'sce_sys\packages' = { $d = & $mkApp 'pkgs'; $null = New-Item -ItemType Directory (Join-Path $d 'sce_sys\packages'); [IO.File]::WriteAllText((Join-Path $d 'sce_sys\packages\x.dat'), 'x'); Get-AppPushPlan $d '' }
            'work.bin anywhere' = { $d = & $mkApp 'work'; [IO.File]::WriteAllText((Join-Path $d 'sce_module\Work.bin'), 'x'); Get-AppPushPlan $d '' }
            '.rif file' = { $d = & $mkApp 'rif'; [IO.File]::WriteAllText((Join-Path $d 'a.RIF'), 'x'); Get-AppPushPlan $d '' }
            'license-format content under another name' = { $d = & $mkApp 'hidden'; [IO.File]::WriteAllBytes((Join-Path $d 'data.bin'), (& $mkLic $stCid)); Get-AppPushPlan $d '' }
            'name with a space' = { $d = & $mkApp 'space'; [IO.File]::WriteAllText((Join-Path $d 'a b.txt'), 'x'); Get-AppPushPlan $d '' }
            'source is not a folder' = { Get-AppPushPlan (Join-Path $tmp 'nope') '' }
        }
        foreach ($k in $refuse.Keys) {
            $msg = $null; try { $null = & $refuse[$k] } catch { $msg = $_.Exception.Message }
            & $assert ($msg -match 'VITOX-HARNESS') "not refused: $k ($msg)" }
        # a 512-byte file that is not license-format (another magic, e.g. a signature blob) stays with the app
        $d = & $mkApp 'sig'; $sig = & $mkLic $stCid; $sig[0] = 0x7F; $sig[1] = 0x44; [IO.File]::WriteAllBytes((Join-Path $d 'sce_sys\sig.bin'), $sig)
        & $assert ((Get-AppPushPlan $d '').Files.Count -eq 4) 'non-license 512-byte file refused'
        "plan ok; $($refuse.Count) invalid sources refused; non-license 512-byte file kept" }
    & $check 'm9a.2c dry run: app-push / license-push print the plan, send nothing, fail closed on mismatch' {
        $dcfg = Join-Path $tmp 'dry-cfg.json'; $cc = Read-Json $Config; $cc.ConsoleUrl = 'https://192.0.2.1:11443'; Write-Json $cc $dcfg
        $app = & $mkApp 'dry'; $licf = Join-Path $tmp 'lic.bin'; [IO.File]::WriteAllBytes($licf, (& $mkLic $stCid))
        $badf = Join-Path $tmp 'badlic.bin'; [IO.File]::WriteAllBytes($badf, (& $mkLic 'XX0000-WXYZ54321_00-0123456789ABCDEF'))
        $run = { param([string[]]$A) $sw = [Diagnostics.Stopwatch]::StartNew(); $o = & $ps -NoProfile -File $PSCommandPath @A -Config $dcfg -DryRun 2>&1
            [pscustomobject]@{ Exit = $LASTEXITCODE; Out = ($o | Out-String); Ms = $sw.ElapsedMilliseconds } }
        $a = & $run @('app-push', '-Source', $app, '-TitleId', $stId)
        & $assert ($a.Exit -eq 0 -and $a.Out -match 'no console traffic' -and ([regex]::Matches($a.Out, 'would upload')).Count -eq 3) "app-push dry run: exit $($a.Exit) $($a.Out)"
        $b = & $run @('app-push', '-Source', $app, '-TitleId', 'WXYZ54321')
        & $assert ($b.Exit -ne 0 -and $b.Out -match 'differs') 'app-push dry run accepted a TITLE_ID mismatch'
        $c = & $run @('license-push', '-License', $licf, '-TitleId', $stId, '-Source', $app)
        & $assert ($c.Exit -eq 0 -and $c.Out -match "licenses\\$stCid\\work\.bin" -and $c.Out -match 'no console traffic') "license-push dry run: exit $($c.Exit) $($c.Out)"
        $e = & $run @('license-push', '-License', $badf, '-TitleId', $stId, '-Source', $app)
        & $assert ($e.Exit -ne 0 -and $e.Out -match 'refused') 'license-push dry run accepted a mismatched license'
        $f = & $run @('license-push', '-License', $licf, '-TitleId', $stId)
        & $assert ($f.Exit -ne 0) 'license-push dry run without -Source accepted'
        foreach ($x in $a, $c) { & $assert ($x.Ms -lt 20000) "dry run took $($x.Ms) ms (console traffic to an unroutable address?)" }
        "app-push 3 planned, mismatch refused; license-push planned, mismatch + no -Source refused; unroutable console never contacted" }
    & $check 'm9a.2c select: by folder, else unique TITLE_ID; rejected/ambiguous/none fail closed' {
        $apps = @([pscustomobject]@{ Folder = $stId; TitleId = $stId; Reject = $null },
            [pscustomobject]@{ Folder = 'WXYZ54321'; TitleId = 'WXYZ54321'; Reject = "app-staging folder name is not its param.sfo TITLE_ID 'QQQQ00000'" },
            [pscustomobject]@{ Folder = 'dupA'; TitleId = 'DUPE00001'; Reject = $null }, [pscustomobject]@{ Folder = 'dupB'; TitleId = 'DUPE00001'; Reject = $null })
        & $assert ((Select-RemoteApp $apps $stId).Hit.Folder -ceq $stId) 'valid app not selected'
        & $assert (-not (Select-RemoteApp $apps 'WXYZ54321').Hit) 'rejected app selected'
        & $assert (-not (Select-RemoteApp $apps 'DUPE00001').Hit -and (Select-RemoteApp $apps 'DUPE00001').Why -match 'ambiguous') 'ambiguous TITLE_ID selected'
        & $assert ((Select-RemoteApp $apps 'dupB').Hit.Folder -ceq 'dupB') 'folder selection failed'
        & $assert (-not (Select-RemoteApp @() $stId).Hit -and -not (Select-RemoteApp $apps 'abcd12345').Hit) 'none / case-folded selected'
        'ok' }
    & $check 'm9a.3a licensed-load: -LicensedLoad only with app-staging + fail-closed + headless, never with diagnostics' {
        & $assert (-not (Get-LicensedLoadError $false '' $false 'visual' $true)) 'off must always be accepted'
        & $assert (-not (Get-LicensedLoadError $true 'app-staging' $true 'headless' $false)) 'the valid combination refused'
        $bad = @(@('', $true, 'headless', $false), @('homebrew', $true, 'headless', $false), @('app-staging', $false, 'headless', $false),
            @('app-staging', $true, 'visual', $false), @('app-staging', $true, 'headless', $true), @('App-Staging', $true, 'headless', $false))
        foreach ($b in $bad) { & $assert (Get-LicensedLoadError $true $b[0] $b[1] $b[2] $b[3]) "accepted root='$($b[0])' closed=$($b[1]) presenter=$($b[2]) diag=$($b[3])" }
        # CLI: refused before any console traffic (unroutable console, so a connection attempt would show as a slow failure)
        $dcfg = Join-Path $tmp 'll-cfg.json'; $cc = Read-Json $Config; $cc.ConsoleUrl = 'https://192.0.2.1:11443'; Write-Json $cc $dcfg
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $o = & $ps -NoProfile -File $PSCommandPath homebrew-select -Config $dcfg -App $stId -AppRoot app-staging -LicensedLoad -Presenter headless 2>&1
        & $assert ($LASTEXITCODE -ne 0 -and ($o | Out-String) -match 'requires -FailClosed' -and $sw.ElapsedMilliseconds -lt 20000) "CLI not refused early: $($o | Out-String)"
        "valid accepted; $($bad.Count) invalid combinations refused; CLI refusal before console traffic" }
    & $check 'm9a.2c: no title-specific IDs in the harness' {
        $src = Get-Content -Raw -LiteralPath $PSCommandPath
        $cids = @([regex]::Matches($src, '[A-Z]{2}[0-9]{4}-[A-Z0-9]{9}_[0-9]{2}-[A-Z0-9]{16}') | ForEach-Object Value | Where-Object { -not $_.StartsWith('XX0000-') })
        & $assert ($cids.Count -eq 0) "real-looking content IDs: $($cids -join ', ')"
        $tids = @([regex]::Matches($src, '\bPC[SE][A-Z][0-9]{5}\b') | ForEach-Object Value)
        & $assert ($tids.Count -eq 0) "retail TITLE_IDs: $($tids -join ', ')"
        'only synthetic XX0000-/ABCD12345 vectors' }

    & $check 'credentials: helper (DPAPI Export-Clixml)' { 'SKIPPED: console auth is disabled and the user said not to create or store credentials' }

    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    $results | Format-Table -AutoSize -Wrap | Out-String -Width 220 | Write-Host
    $fail = @($results | Where-Object Result -eq 'FAIL').Count
    Write-Host ("SELFTEST {0}: {1} pass, {2} fail" -f $(if ($fail) { 'FAIL' } else { 'PASS' }), (@($results | Where-Object Result -eq 'PASS').Count), $fail)
    $null = New-Item -ItemType Directory -Force $script:StateDir
    Write-Json @{ at = (Get-Date).ToString('o'); harness_version = $script:HarnessVersion; results = $results } (Join-Path $script:StateDir 'last-selftest.json')
    if ($fail) { exit 1 }
    exit 0  # child processes in the tests leave a non-zero $LASTEXITCODE behind on purpose
}

switch ($Command) {
    'selftest' { Invoke-Selftest }
    'status' { Invoke-Status }
    'trust' { Invoke-Trust }
    'discover' { Invoke-Discover }
    'prepare' { Invoke-Prepare }
    'launch' { Invoke-Launch }
    'collect' { Invoke-Collect }
    'stop' { Invoke-Stop }
    'homebrew-list' { Invoke-HomebrewList }
    'homebrew-push' { Invoke-HomebrewPush }
    'homebrew-select' { Invoke-HomebrewSelect }
    'firmware-push' { Invoke-FirmwarePush }
    'app-push' { Invoke-AppPush }
    'license-push' { Invoke-LicensePush }
    'data-backup' { Invoke-DataBackup }
    'screenshot' { Invoke-Screenshot }
    'uninstall' { Invoke-Uninstall }
}
