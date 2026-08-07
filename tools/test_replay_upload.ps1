<#
.SYNOPSIS
    Mimics the mod's UploadReplayBinary call (src/Web/url_downloader.cpp) at the raw socket
    level, so upload failures can be reproduced and iterated on without rebuilding/redeploying
    the mod itself.

.DESCRIPTION
    The mod does, via WinINet:
        InternetConnect(host, port, INTERNET_SERVICE_HTTP, flags=0)
        HttpOpenRequest(POST, endpoint, INTERNET_FLAG_SECURE if UploadReplayDataUseTls else 0)
        HttpAddRequestHeaders("Content-Type: application/octet-stream")
        HttpSendRequest(..., 64KiB replay buffer)

    This script opens a raw TCP socket (optionally wrapped in TLS, see $UseTls below) and writes
    a hand-built HTTP/1.1 POST request byte-for-byte, so it fails/succeeds under the exact same
    conditions WinINet would - no higher-level HTTP client behavior (redirects, retries, protocol
    upgrades) to mask what's actually happening on the wire.

    By default it runs the request TWICE: once with a small body, then once with the real 64KiB
    body. That distinguishes the two very different failure modes seen in the field:
      - both fail    -> the host/path is unreachable, or the response is being blocked outright
      - small works,
        64KiB fails  -> the connection only breaks on bulk data. Classic MTU black hole (broken
                        path-MTU discovery, blocked ICMP, a tunnel/VPN) or deep-packet-inspection
                        dropping large uploads. The handshakes are small enough to pass, then the
                        full-size data packets vanish and TCP gives up retransmitting (~19s).

    IMPORTANT when reading the output: any HTTP response at all counts as reaching the server -
    even 400 or 500. A 400/500 means the network path works and the server merely disliked the
    body (expected for the random test payloads, which are not valid replays). Only a timeout or
    connection error means the traffic never completed the round trip.

    HOW TO RUN THIS (no console/typing needed):
      1. Change the values in the "SETTINGS" block below if asked to (leave them alone otherwise).
      2. Save the file.
      3. Right-click the file -> "Run with PowerShell" (on some setups this is under a "Show
         more options" submenu on Windows 11).
      4. A black window will open, run the tests, print a summary, and wait for a key press
         before closing - so nothing disappears before it can be read/screenshotted.
#>

# ============================== SETTINGS ==============================
# Change these values if asked to, then save the file and re-run it.

$HostName              = "replays.blazqueue.com"  # server to test against
$Port                  = 443                      # port to test against
$Endpoint              = "/upload"                # request path
$UseTls                = $true                    # $true = real HTTPS/TLS, $false = plain HTTP
$PayloadFilePath       = ""                       # path to a real replay .dat to upload, e.g. "D:\...\Save\Replay\archive\foo.dat"
                                                  # leave blank ("") to send random bytes instead
$PayloadSizeBytes      = 65536                    # size of the random main payload when $PayloadFilePath is blank (64KiB = real replay size)
$AlsoTestSmallRequest  = $true                    # also send a small body first, to see whether failure depends on size
$SmallPayloadSizeBytes = 1024                     # size of that small test body
$TimeoutSeconds        = 30                       # how long to wait before giving up on a step

# ========================================================================

function Log([string]$msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff")
    Write-Host "[$ts] $msg"
}

function Format-Elapsed([System.Diagnostics.Stopwatch]$sw) {
    return "{0:N3}s" -f $sw.Elapsed.TotalSeconds
}

# Runs one full request. Returns $true if ANY HTTP response came back (even 4xx/5xx), else $false.
# Never exits the script - both rounds must run so their results can be compared.
function Invoke-UploadTest([string]$label, [byte[]]$payload) {
    Write-Host ""
    Log "===== $label : $($payload.Length) byte body, tls: $UseTls ====="

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
                return $false
            }
        } catch {
            $inner = $_.Exception.InnerException
            $msg = if ($inner) { $inner.Message } else { $_.Exception.Message }
            Log "[error] Failed to connect after $(Format-Elapsed $connectSw). $msg"
            return $false
        }
        Log "TCP connected in $(Format-Elapsed $connectSw)."

        $stream = $client.GetStream()

        if ($UseTls) {
            $tlsSw = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $sslStream = New-Object System.Net.Security.SslStream($stream, $false)
                $sslStream.AuthenticateAsClient($HostName)
                $stream = $sslStream
                Log "TLS handshake completed in $(Format-Elapsed $tlsSw). Protocol: $($sslStream.SslProtocol)"
            } catch {
                Log "[error] TLS handshake failed after $(Format-Elapsed $tlsSw). $($_.Exception.Message)"
                return $false
            }
        }

        # Same request shape the mod builds: POST <endpoint>, Content-Type: application/octet-stream.
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
            # NOTE: this only means the bytes were handed to the OS/TLS buffer. It does NOT mean
            # they reached the server - a body this size fits in local socket buffers and returns
            # instantly even when every packet is being dropped further along the path.
            Log "Request written ($($headerBytes.Length + $payload.Length) bytes) in $(Format-Elapsed $sendSw) - handed to the OS, delivery not yet confirmed."
        } catch {
            Log "[error] Failed to write request after $(Format-Elapsed $sendSw). $($_.Exception.Message)"
            return $false
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
            Log "RESULT: reached the server and got a response (any status counts as the network working)."
            return $true
        } catch {
            Log "[error] No response. Failed after $(Format-Elapsed $readSw). $($_.Exception.Message)"
            Log "RESULT: the request never completed a round trip."
            return $false
        }
    } finally {
        $client.Close()
    }
}

Log "UploadReplayBinary (simulated)."
Log "`thost: '$HostName'"
Log "`tendpoint: '$Endpoint'"
Log "`tport: $Port"
Log "`ttls: $UseTls"

$smallOk = $null
if ($AlsoTestSmallRequest) {
    $smallPayload = New-Object byte[] $SmallPayloadSizeBytes
    (New-Object System.Random).NextBytes($smallPayload)
    $smallOk = Invoke-UploadTest "TEST 1 of 2 - small body" $smallPayload
}

if ($PayloadFilePath -ne "") {
    if (-not (Test-Path $PayloadFilePath)) {
        Log "[error] PayloadFilePath does not exist: '$PayloadFilePath'"
        Write-Host ""
        Write-Host "Press Enter to close this window..."
        Read-Host | Out-Null
        exit 1
    }
    $mainPayload = [System.IO.File]::ReadAllBytes($PayloadFilePath)
    Log "Main payload loaded from '$PayloadFilePath'."
} else {
    $mainPayload = New-Object byte[] $PayloadSizeBytes
    (New-Object System.Random).NextBytes($mainPayload)
}
$mainOk = Invoke-UploadTest "TEST 2 of 2 - full-size body" $mainPayload

Write-Host ""
Log "===== SUMMARY ====="
if ($AlsoTestSmallRequest) {
    Log "small body ($SmallPayloadSizeBytes bytes): $(if ($smallOk) { 'got a response' } else { 'NO response' })"
}
Log "full body ($($mainPayload.Length) bytes): $(if ($mainOk) { 'got a response' } else { 'NO response' })"

if ($AlsoTestSmallRequest -and $smallOk -and -not $mainOk) {
    Log "=> Size-dependent failure: small requests get through, large ones do not."
    Log "   That points at the network path dropping bulk data (MTU black hole / DPI), not at the server."
} elseif ($AlsoTestSmallRequest -and -not $smallOk -and -not $mainOk) {
    Log "=> Nothing gets a response at all, regardless of size - the host/port is unreachable from here."
} elseif ($mainOk) {
    Log "=> Uploads work from this machine with these settings."
}

Write-Host ""
Write-Host "Press Enter to close this window..."
Read-Host | Out-Null
