<#
.SYNOPSIS
    Mimics the mod's UploadReplayBinary call (src/Web/url_downloader.cpp) at the raw socket
    level, so upload failures can be reproduced and iterated on without rebuilding/redeploying
    the mod itself.

.DESCRIPTION
    The mod currently does, via WinINet:
        InternetConnect(host, port, INTERNET_SERVICE_HTTP, flags=0)
        HttpOpenRequest(POST, endpoint, flags=0)                    <- flags=0 means NOT secure
        HttpAddRequestHeaders("Content-Type: application/octet-stream")
        HttpSendRequest(..., 64KiB replay buffer)

    This script opens a raw TCP socket (optionally wrapped in TLS, see $UseTls below) and writes
    a hand-built HTTP/1.1 POST request byte-for-byte, so it fails/succeeds under the exact same
    conditions WinINet would - no higher-level HTTP client behavior (redirects, retries, protocol
    upgrades) to mask what's actually happening on the wire.

    HOW TO RUN THIS (no console/typing needed):
      1. Change the values in the "SETTINGS" block below if asked to (leave them alone otherwise).
      2. Save the file.
      3. Right-click the file -> "Run with PowerShell" (on some setups this is under a "Show
         more options" submenu on Windows 11).
      4. A black window will open, run the test, print the result, and wait for a key press
         before closing - so nothing disappears before it can be read/screenshotted.
#>

# ============================== SETTINGS ==============================
# Change these values if asked to, then save the file and re-run it.

$HostName         = "replays.blazqueue.com"   # server to test against
$Port             = 443                       # port to test against
$Endpoint         = "/upload"                 # request path
$UseTls           = $false                    # $true = use real HTTPS/TLS, $false = plain HTTP (what the mod does today)
$PayloadFilePath  = "D:\SteamLibrary\steamapps\common\BlazBlue Centralfiction\Save\Replay\archive\260214T20_17_Hishe_V_Kam.dat"                        # path to a real replay .dat file to upload, e.g. "C:\...\Save\Replay\archive\somefile.dat"
                                               # leave blank ("") to send 65536 random bytes instead (fine for connection testing, but
                                               # a real backend may reject/error on garbage data - use a real file to test genuinely)
$PayloadSizeBytes = 65536                     # size of the random payload when $PayloadFilePath is blank
$TimeoutSeconds   = 30                        # how long to wait before giving up on a step

# ========================================================================

function Log([string]$msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff")
    Write-Host "[$ts] $msg"
}

function Format-Elapsed([System.Diagnostics.Stopwatch]$sw) {
    return "{0:N3}s" -f $sw.Elapsed.TotalSeconds
}

function Stop-WithPause {
    Write-Host ""
    Write-Host "Press Enter to close this window..."
    Read-Host | Out-Null
    exit 1
}

Log "UploadReplayBinary (simulated) connecting."
Log "`thost: '$HostName'"
Log "`tendpoint: '$Endpoint'"
Log "`tport: $Port"
Log "`ttls: $UseTls"

$overallSw = [System.Diagnostics.Stopwatch]::StartNew()
$client = New-Object System.Net.Sockets.TcpClient
$client.ReceiveTimeout = $TimeoutSeconds * 1000
$client.SendTimeout = $TimeoutSeconds * 1000

try {
    $connectSw = [System.Diagnostics.Stopwatch]::StartNew()
    # Task.Wait() throws (wrapped in an AggregateException) if the task faults - e.g. connection
    # refused - rather than just returning false, so that has to be caught here too, not just
    # the plain timeout-elapsed case where Wait() returns false with no exception.
    try {
        $connectTask = $client.ConnectAsync($HostName, $Port)
        if (-not $connectTask.Wait($TimeoutSeconds * 1000)) {
            Log "[error] Failed to connect. Timed out after $(Format-Elapsed $connectSw) (TCP handshake never completed)."
            Stop-WithPause
        }
    } catch {
        $inner = $_.Exception.InnerException
        $msg = if ($inner) { $inner.Message } else { $_.Exception.Message }
        Log "[error] Failed to connect after $(Format-Elapsed $connectSw). $msg"
        Stop-WithPause
    }
    Log "TCP connected in $(Format-Elapsed $connectSw)."

    $networkStream = $client.GetStream()
    $stream = $networkStream

    if ($UseTls) {
        $tlsSw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $sslStream = New-Object System.Net.Security.SslStream($networkStream, $false)
            $sslStream.AuthenticateAsClient($HostName)
            $stream = $sslStream
            Log "TLS handshake completed in $(Format-Elapsed $tlsSw). Protocol: $($sslStream.SslProtocol)"
        } catch {
            Log "[error] TLS handshake failed after $(Format-Elapsed $tlsSw). $($_.Exception.Message)"
            Stop-WithPause
        }
    }

    # Build the request exactly like the mod does: POST <endpoint>, Content-Type:
    # application/octet-stream, body = either a real replay file's bytes or a random buffer
    # standing in for one - random bytes are fine for testing the connection itself, but a
    # server that validates the replay format may reject/error on them where it wouldn't on
    # a real file, so that has to be ruled out separately before blaming the server.
    if ($PayloadFilePath -ne "") {
        if (-not (Test-Path $PayloadFilePath)) {
            Log "[error] PayloadFilePath does not exist: '$PayloadFilePath'"
            Stop-WithPause
        }
        $payload = [System.IO.File]::ReadAllBytes($PayloadFilePath)
        Log "Loaded payload from '$PayloadFilePath' ($($payload.Length) bytes)."
    } else {
        $payload = New-Object byte[] $PayloadSizeBytes
        (New-Object System.Random).NextBytes($payload)
        Log "Using $($payload.Length) random bytes as payload (set `$PayloadFilePath to use a real replay file instead)."
    }

    $headerText = @(
        "POST $Endpoint HTTP/1.1",
        "Host: $HostName",
        "Content-Type: application/octet-stream",
        "Content-Length: $($payload.Length)",
        "Connection: close",
        "",
        ""
    ) -join "`r`n"
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($headerText)

    $sendSw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $stream.Write($headerBytes, 0, $headerBytes.Length)
        $stream.Write($payload, 0, $payload.Length)
        $stream.Flush()
        Log "Request sent ($($headerBytes.Length + $payload.Length) bytes) in $(Format-Elapsed $sendSw)."
    } catch {
        Log "[error] Failed to send request. Failed after $(Format-Elapsed $sendSw). $($_.Exception.Message)"
        Stop-WithPause
    }

    $readSw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $reader = New-Object System.IO.StreamReader($stream)
        $statusLine = $reader.ReadLine()
        $responseHeaders = @()
        while (($line = $reader.ReadLine()) -and $line -ne "") {
            $responseHeaders += $line
        }
        $body = $reader.ReadToEnd()

        Log "Response received in $(Format-Elapsed $readSw)."
        Log "Status line: $statusLine"
        Log "Headers:"
        $responseHeaders | ForEach-Object { Log "`t$_" }
        Log "Body: $body"
    } catch {
        Log "[error] Failed to read response. Failed after $(Format-Elapsed $readSw). $($_.Exception.Message)"
        Stop-WithPause
    }

    Log "UploadReplayBinary (simulated) successful. Total time: $(Format-Elapsed $overallSw)"
} finally {
    $client.Close()
}

Write-Host ""
Write-Host "Press Enter to close this window..."
Read-Host | Out-Null
