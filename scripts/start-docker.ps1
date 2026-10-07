$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
$composeFile = Join-Path $projectDirectory 'compose.yaml'

function Get-DockerEngine {
    # Windows PowerShell treats a stopped engine's stderr as an error record.
    $ErrorActionPreference = 'Continue'
    $result = & docker info --format '{{.OSType}}' 2>$null
    if ($LASTEXITCODE -eq 0) { return $result }
    return $null
}

try {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker Desktop is required. Install it, then run this command again.'
    }
    $engineType = Get-DockerEngine
    if (-not $engineType) {
        Write-Host 'Starting Docker Desktop...'
        & docker desktop start --timeout 120
        if ($LASTEXITCODE -ne 0) { throw 'Docker Desktop could not start. Open Docker Desktop and check its startup error.' }
        $deadline = (Get-Date).AddSeconds(120)
        do {
            $engineType = Get-DockerEngine
            if ($engineType) { break }
            Start-Sleep -Seconds 2
        } while ((Get-Date) -lt $deadline)
        if (-not $engineType) { throw 'Docker engine did not become ready within 120 seconds.' }
    }
    if ($engineType -ne 'linux') { throw 'Switch Docker Desktop to Linux containers, then run this command again.' }
    Write-Host 'Building and starting SwiftKV (the first build downloads dependencies)...'
    & docker compose -f $composeFile up --build --detach --wait --wait-timeout 120
    if ($LASTEXITCODE -ne 0) {
        throw 'SwiftKV failed to start. Check the Docker output above; ports 6380 and 6381 must be free.'
    }
    Write-Host ''
    Write-Host 'SwiftKV is ready.' -ForegroundColor Green
    Write-Host 'Dashboard: http://localhost:6381'
    Write-Host 'Database:  localhost:6380 (RESP protocol)'
    Write-Host 'Data is retained in the swiftkv-local_swiftkv-data Docker volume.'
    exit 0
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
