$ErrorActionPreference = 'Stop'
$composeFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'compose.yaml'

function Assert-Reply([string[]] $Command, [string] $Expected) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $client.Connect('127.0.0.1', 6380)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $stream.WriteTimeout = 5000
        $frame = '*' + $Command.Length + "`r`n"
        foreach ($part in $Command) {
            $frame += '$' + [Text.Encoding]::UTF8.GetByteCount($part) + "`r`n" + $part + "`r`n"
        }
        $bytes = [Text.Encoding]::UTF8.GetBytes($frame)
        $stream.Write($bytes, 0, $bytes.Length)
        $expectedBytes = [Text.Encoding]::UTF8.GetBytes($Expected)
        $buffer = New-Object byte[] $expectedBytes.Length
        $offset = 0
        while ($offset -lt $buffer.Length) {
            $count = $stream.Read($buffer, $offset, $buffer.Length - $offset)
            if ($count -eq 0) { throw 'Server disconnected before replying.' }
            $offset += $count
        }
        $actual = [Text.Encoding]::UTF8.GetString($buffer)
        if ($actual -cne $Expected) { throw "Unexpected reply to $($Command[0]): $actual" }
    } finally { $client.Dispose() }
}

$testKey = 'docker-smoke:' + [Guid]::NewGuid().ToString('N')
try {
    Assert-Reply @('PING') "+PONG`r`n"
    Assert-Reply @('SET', $testKey, 'persistent-value') "+OK`r`n"
    Assert-Reply @('GET', $testKey) ('$16' + "`r`npersistent-value`r`n")
    Assert-Reply @('EXISTS', $testKey) ":1`r`n"
    $health = Invoke-RestMethod 'http://localhost:6381/health'
    if ($health.status -ne 'ok') { throw 'HTTP health failed.' }
    $dashboard = Invoke-WebRequest 'http://localhost:6381/' -UseBasicParsing
    if ($dashboard.Content -notmatch '<title>SwiftKV') { throw 'Dashboard HTML missing.' }
    Write-Host 'PASS: PING, SET, GET, EXISTS, health and dashboard.'

    & docker compose -f $composeFile restart swiftkv
    if ($LASTEXITCODE -ne 0) { throw 'Container restart failed.' }
    & docker compose -f $composeFile up --detach --wait --wait-timeout 60
    if ($LASTEXITCODE -ne 0) { throw 'Restart readiness failed.' }
    Assert-Reply @('GET', $testKey) ('$16' + "`r`npersistent-value`r`n")
    Write-Host 'PASS: data survived container restart.'

    & docker compose -f $composeFile up --detach --force-recreate --wait --wait-timeout 60
    if ($LASTEXITCODE -ne 0) { throw 'Container recreation failed.' }
    Assert-Reply @('GET', $testKey) ('$16' + "`r`npersistent-value`r`n")
    Write-Host 'PASS: data survived container replacement using the named volume.'
    Assert-Reply @('DEL', $testKey) ":1`r`n"
    Assert-Reply @('GET', $testKey) ('$-1' + "`r`n")
    Write-Host 'PASS: DEL and missing-key response; test key removed.'
} finally {
    # Remove only this run's unique key, including after an assertion failure.
    try { Assert-Reply @('DEL', $testKey) ":0`r`n" } catch { }
}
exit 0
