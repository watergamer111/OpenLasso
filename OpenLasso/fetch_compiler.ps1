# Looks for a portable C++ compiler in .\toolchain (writes the bin path to toolchain\bin_path.txt)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls13
$ProgressPreference = 'SilentlyContinue'
$dest = Join-Path $PSScriptRoot 'toolchain'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$pathFile = Join-Path $dest 'bin_path.txt'

function Download($url, $out) {
    try { Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing -Headers @{ 'User-Agent' = 'OpenLasso-build' }; return }
    catch { Write-Host "  Invoke-WebRequest failed: $($_.Exception.Message)" }
    if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
        Write-Host '  Retrying with curl.exe...'
        & curl.exe -L --fail -A 'OpenLasso-build' -o $out $url
        if ($LASTEXITCODE -eq 0 -and (Test-Path $out)) { return }
    }
    Write-Host '  Retrying with BITS...'
    Start-BitsTransfer -Source $url -Destination $out
}

# --- Method 1: w64devkit from GitHub ---
try {
    Write-Host '[1/2] Looking for w64devkit on GitHub...'
    $rel = Invoke-RestMethod 'https://api.github.com/repos/skeeto/w64devkit/releases/latest' -Headers @{ 'User-Agent' = 'OpenLasso-build' }
    Write-Host ('  Files in the release: ' + (($rel.assets | ForEach-Object { $_.name }) -join ', '))
    $cands = $rel.assets | Where-Object { $_.name -like 'w64devkit*' -and $_.name -notlike '*fortran*' -and $_.name -notlike '*i686*' }
    $asset = $cands | Where-Object { $_.name -like '*.zip' } | Select-Object -First 1
    if (-not $asset) { $asset = $cands | Where-Object { $_.name -like '*.exe' } | Select-Object -First 1 }
    if (-not $asset) { throw 'No compatible .zip/.exe package in the release.' }
    $file = Join-Path $dest $asset.name
    Write-Host "  Downloading $($asset.name) ($([math]::Round($asset.size/1MB)) MB)..."
    Download $asset.browser_download_url $file
    if ($file -like '*.zip') { Expand-Archive -Path $file -DestinationPath $dest -Force }
    else { & $file "-o$dest" -y | Out-Null }
    Remove-Item $file -Force -ErrorAction SilentlyContinue
    $gpp = Get-ChildItem -Path $dest -Recurse -Filter g++.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gpp) { throw 'g++.exe not found after extraction.' }
    Set-Content -Path $pathFile -Value $gpp.DirectoryName -Encoding ASCII
    Write-Host "Compiler ready: $($gpp.DirectoryName)"
    exit 0
} catch {
    Write-Host "  Method 1 failed: $($_.Exception.Message)" -ForegroundColor Yellow
}

# --- Method 2: winget (WinLibs) ---
try {
    Write-Host '[2/2] Trying winget (WinLibs MinGW-w64)...'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { throw 'winget is not available on this PC.' }
    & winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT --accept-package-agreements --accept-source-agreements
    $roots = @("$env:LOCALAPPDATA\Microsoft\WinGet\Packages", "$env:ProgramFiles\WinGet\Packages")
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $gpp = Get-ChildItem -Path $r -Recurse -Filter g++.exe -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($gpp) { Set-Content -Path $pathFile -Value $gpp.DirectoryName -Encoding ASCII; Write-Host "Compiler ready: $($gpp.DirectoryName)"; exit 0 }
        }
    }
    throw 'g++.exe not found after the winget installation.'
} catch {
    Write-Host "  Method 2 failed: $($_.Exception.Message)" -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'Automatic download failed. Install ONE of these by hand, then run build.bat again:' -ForegroundColor Red
Write-Host '  - https://github.com/skeeto/w64devkit/releases  (download, extract, add the bin folder to PATH)'
Write-Host '  - Build Tools for Visual Studio (workload "Desktop development with C++")'
exit 1
