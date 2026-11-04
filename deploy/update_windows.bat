<# : batch part: runs the PowerShell below (this one file is both)
@echo off
setlocal
set "RD_ARGS=%*"
set "RD_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "iex ([IO.File]::ReadAllText($env:RD_SELF))"
set "rc=%errorlevel%"
if not defined RD_NOPAUSE pause
exit /b %rc%
#>
# Install or update RemoDroid on this Windows computer, and set up what it needs.
#
#   update_windows.bat              the latest release
#   update_windows.bat 0.6.2        that release
#   update_windows.bat file.zip     a RemoDroid-Computer-...-windows-....zip you already have
#   update_windows.bat --force      reinstall even when this version is installed
#
# Double-click it, or run it in a terminal. It updates RemoDroid where it is (the running one, or the
# one that starts at login), else installs it in %LOCALAPPDATA%\Programs\RemoDroid, and restarts it.
# With one administrator prompt (UAC) it then:
#   - installs or updates the RemoDroidInput service, so RemoDroid can control windows of programs
#     that run as administrator, and the lock screen (asked the first time; kept up to date after)
#   - lets RemoDroid through Windows Firewall, so the video can come straight from this computer
#     (peer to peer) instead of through the server's relay
# Start at login is turned on for a new install and kept pointing at the right place after.
# Pairing and settings (%APPDATA%\RemoDroid) stay.

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Windows PowerShell's progress bar makes downloads crawl
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repo = 'yinyue123/RemoDroid'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function Say($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }
function Die($m) { Write-Host "xx  $m" -ForegroundColor Red; exit 1 }

# ---------------- arguments ----------------
$force = $false; $want = ''
$argv = @()
if ($env:RD_ARGS) { $argv = [regex]::Matches($env:RD_ARGS, '"([^"]*)"|(\S+)') | ForEach-Object { if ($_.Groups[1].Success) { $_.Groups[1].Value } else { $_.Groups[2].Value } } }
foreach ($a in $argv) {
    if ($a -in '--force', '-f', '/f') { $force = $true }
    elseif ($a -in '--help', '-h', '/?') { Get-Content -LiteralPath $env:RD_SELF | Select-Object -Skip 10 -First 16 | ForEach-Object { $_ -replace '^# ?', '' }; exit 0 }
    else { $want = $a }
}

$arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'arm64' } else { 'x86_64' }

# ---------------- where it is ----------------
$running = Get-Process -Name RemoDroid -ErrorAction SilentlyContinue | Where-Object { $_.Path } | Select-Object -First 1
$runValue = (Get-ItemProperty -Path $runKey -Name RemoDroid -ErrorAction SilentlyContinue).RemoDroid
$dir = $null
if ($running) { $dir = Split-Path $running.Path }
elseif ($runValue -and $runValue -match '^"([^"]+)"') { $dir = Split-Path $Matches[1] }
$fresh = -not ($dir -and (Test-Path (Join-Path $dir 'RemoDroid.exe')))
if ($fresh) { $dir = Join-Path $env:LOCALAPPDATA 'Programs\RemoDroid' }
$verFile = Join-Path $dir 'version.txt'
$installed = if (Test-Path $verFile) { (Get-Content $verFile -Raw).Trim() } else { '' }

# ---------------- which build ----------------
$zip = $null; $ver = ''
if ($want -and (Test-Path -LiteralPath $want -PathType Leaf)) {
    $zip = (Resolve-Path -LiteralPath $want).Path
    if ((Split-Path $zip -Leaf) -match '^RemoDroid-Computer-(.+)-windows-[a-z0-9_]+\.zip$') { $ver = $Matches[1] }
} elseif ($want) {
    $ver = $want.TrimStart('v')
} else {
    Say 'Looking up the latest release'
    try {   # the release page redirects to .../releases/tag/vX.Y.Z (no API rate limit)
        $req = [Net.HttpWebRequest]::Create("https://github.com/$repo/releases/latest")
        $req.AllowAutoRedirect = $false
        $resp = $req.GetResponse(); $loc = $resp.Headers['Location']; $resp.Close()
    } catch { Die "can't reach github.com: $($_.Exception.Message)" }
    if ($loc -notmatch '/tag/v(.+)$') { Die "no release found ($loc)" }
    $ver = $Matches[1]
}
Say "Installed: $(if ($installed) { $installed } elseif ($fresh) { 'none' } else { 'unknown version' })   wanted: $(if ($ver) { $ver } else { $zip })   in $dir"

# ---------------- download and put in place ----------------
$tmp = Join-Path $env:TEMP ("remodroid-update-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$copied = $false
try {
    if ($zip -or $force -or $fresh -or $installed -ne $ver) {
        if (-not $zip) {
            $name = "RemoDroid-Computer-$ver-windows-$arch.zip"
            Say "Downloading $name"
            $zip = Join-Path $tmp $name
            try { Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/$repo/releases/download/v$ver/$name" -OutFile $zip }
            catch { Die "download failed: https://github.com/$repo/releases/download/v$ver/$name ($($_.Exception.Message))" }
        }
        Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $tmp 'x') -Force
        $src = Get-ChildItem -Path (Join-Path $tmp 'x') -Recurse -Filter RemoDroid.exe | Select-Object -First 1
        if (-not $src) { Die "no RemoDroid.exe in $zip" }
        $src = $src.DirectoryName

        $stop = Get-Process -Name RemoDroid, remodroid-desktopd -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and ((Split-Path $_.Path) -eq $dir) }
        if ($stop) {
            Say 'Stopping RemoDroid'
            $stop | Stop-Process -Force
            $stop | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
        }
        New-Item -ItemType Directory -Path $dir -Force -ErrorAction SilentlyContinue | Out-Null
        try {
            Copy-Item -Path (Join-Path $src '*') -Destination $dir -Force
            Set-Content -Path $verFile -Value $ver -Encoding ASCII
            $copied = $true
        } catch {
            Warn "can't write to $dir as you ($($_.Exception.Message)): copying as administrator"
        }
        Say "Installed in $dir"
    } else {
        Say "RemoDroid $ver is already installed (--force reinstalls)"
        $src = $null
    }

    # ---------------- what needs an administrator: one UAC prompt ----------------
    $exe = Join-Path $dir 'RemoDroid.exe'
    $inputExe = Join-Path $dir 'remodroid-input.exe'
    $svc = Get-Service -Name RemoDroidInput -ErrorAction SilentlyContinue
    # installed: bring its copy up to date after an update, or start it again; not installed: ask
    $wantSvc = $svc -and ($src -or $svc.Status -ne 'Running')
    if (-not $svc -and (Test-Path $inputExe)) {
        Write-Host ''
        Write-Host 'RemoDroid can also control windows of programs that run as administrator, and the lock screen,'
        Write-Host 'with its RemoDroidInput service. Without it those windows ignore the mouse and keyboard.'
        $ans = Read-Host 'Install it? [Y/n]'
        $wantSvc = $ans -notmatch '^[nN]'
    }
    # an Allow rule for RemoDroid.exe; a Block one (Cancel on Windows' first "allow access?" alert) wins over it
    $rules = @(Get-NetFirewallApplicationFilter -Program $exe -ErrorAction SilentlyContinue |
        Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.Direction -eq 'Inbound' -and $_.Enabled -eq 'True' })
    $wantRule = -not ($rules | Where-Object { $_.Action -eq 'Allow' }) -or [bool]($rules | Where-Object { $_.Action -eq 'Block' })

    $lines = @()
    if (-not $copied -and $src) {
        $lines += "New-Item -ItemType Directory -Path '$dir' -Force | Out-Null"
        $lines += "Copy-Item -Path '$(Join-Path $src '*')' -Destination '$dir' -Force"
        $lines += "Set-Content -Path '$verFile' -Value '$ver' -Encoding ASCII"
    }
    if ($wantSvc) { $lines += "& '$inputExe' install | Out-Host" }
    if ($wantRule) {
        $lines += "Get-NetFirewallApplicationFilter -Program '$exe' -ErrorAction SilentlyContinue | Get-NetFirewallRule | Where-Object { `$_.Direction -eq 'Inbound' -and `$_.Action -eq 'Block' } | Remove-NetFirewallRule"
        $lines += "Get-NetFirewallRule -DisplayName 'RemoDroid' -ErrorAction SilentlyContinue | Remove-NetFirewallRule"
        $lines += "New-NetFirewallRule -DisplayName 'RemoDroid' -Description 'RemoDroid: video and input straight from this computer (peer to peer)' -Direction Inbound -Program '$exe' -Action Allow -Profile Any | Out-Null"
    }
    if ($lines.Count) {
        $what = @()
        if (-not $copied -and $src) { $what += "install in $dir" }
        if ($wantSvc) { $what += $(if ($svc) { 'update the RemoDroidInput service' } else { 'install the RemoDroidInput service' }) }
        if ($wantRule) { $what += 'allow RemoDroid through Windows Firewall' }
        Say "As administrator: $($what -join ', ') (Windows asks)"
        $ps1 = Join-Path $tmp 'admin.ps1'
        Set-Content -Path $ps1 -Encoding UTF8 -Value (@('$ErrorActionPreference = ''Continue''') + $lines)
        try {
            $p = Start-Process -FilePath powershell -Verb RunAs -Wait -PassThru -WindowStyle Hidden `
                -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ps1`""
            if ($p.ExitCode -ne 0) { Warn "the administrator step ended with code $($p.ExitCode)" }
        } catch {
            Warn 'the administrator step was declined'
            if (-not $copied -and $src) { Die "RemoDroid could not be installed in $dir" }
        }
        if ($wantSvc) {
            $s = Get-Service -Name RemoDroidInput -ErrorAction SilentlyContinue
            if ($s -and $s.Status -eq 'Running') { Say 'RemoDroidInput service: running' } else { Warn 'RemoDroidInput service is not running' }
        }
    }

    # ---------------- start at login, and start ----------------
    $cmd = "`"$exe`" --background"
    if ($fresh -or $runValue) {
        if ($runValue -ne $cmd) { Set-ItemProperty -Path $runKey -Name RemoDroid -Value $cmd; Say 'Starts at login' }
    }
    $lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\RemoDroid.lnk'
    if ($fresh -or -not (Test-Path $lnk)) {   # in Start, as an installer would
        $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
        $sc.TargetPath = $exe; $sc.WorkingDirectory = $dir; $sc.Save()
    }
    $log = Join-Path $env:APPDATA 'RemoDroid\desktop.log'
    $before = if (Test-Path $log) { (Get-Content $log).Count } else { 0 }
    if (-not (Get-Process -Name RemoDroid -ErrorAction SilentlyContinue)) {
        Say 'Starting RemoDroid'
        if ($fresh) { Start-Process -FilePath $exe } else { Start-Process -FilePath $exe -ArgumentList '--background' }
    }
    $ok = $false
    for ($i = 0; $i -lt 15 -and -not $ok; $i++) {
        Start-Sleep -Seconds 1
        if (Test-Path $log) { $ok = [bool]((Get-Content $log | Select-Object -Skip $before) -match 'connected to the server') }
    }
    if (Test-Path $log) { Get-Content $log | Select-Object -Skip $before | Select-Object -Last 5 | ForEach-Object { "    $_" } }
    if ($ok) { Say "RemoDroid $ver is running and connected" }
    elseif ($fresh) { Say "RemoDroid $ver is running: pair it in its window" }
    else { Warn "RemoDroid is running but hasn't connected yet: see $log" }
} finally {
    Remove-Item -Recurse -Force -Path $tmp -ErrorAction SilentlyContinue
}
