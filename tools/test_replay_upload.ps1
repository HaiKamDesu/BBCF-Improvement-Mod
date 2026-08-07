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
$AlsoTestChunkedSend   = $true                    # also send the full body in small pieces, to see if that survives where one big write does not
$ChunkSizeBytes        = 1000                     # size of each piece for that chunked test (kept well under a typical 1500-byte MTU)
$AlsoProbePathMtu      = $true                    # best-effort ping probe to find the largest packet that reaches the host
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
# Never exits the script - every round must run so their results can be compared.
# $chunkSize 0 = write the whole body in one call (what the mod does today); >0 = write it in
# pieces that size, with Nagle disabled so each piece leaves as its own small TCP packet. If a
# path drops full-size packets but passes small ones, only the chunked form survives.
function Invoke-UploadTest([string]$label, [byte[]]$payload, [int]$chunkSize = 0) {
    Write-Host ""
    Log "===== $label : $($payload.Length) byte body, tls: $UseTls, chunk: $(if ($chunkSize -gt 0) { "$chunkSize bytes" } else { 'one write' }) ====="

    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = $TimeoutSeconds * 1000
    $client.SendTimeout = $TimeoutSeconds * 1000
    if ($chunkSize -gt 0) {
        # Without this, TCP coalesces the small writes back into full-size segments and the
        # test would be indistinguishable from the single-write case.
        $client.NoDelay = $true
    }

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
            if ($chunkSize -gt 0) {
                $offset = 0
                while ($offset -lt $payload.Length) {
                    $n = [Math]::Min($chunkSize, $payload.Length - $offset)
                    $stream.Write($payload, $offset, $n)
                    $stream.Flush()
                    $offset += $n
                }
            } else {
                $stream.Write($payload, 0, $payload.Length)
                $stream.Flush()
            }
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

# Best-effort largest-packet probe. Uses ping's exit code rather than its printed text, since
# that text is localized and unparseable across languages. Returns the largest ICMP payload that
# got through, or $null if ICMP is blocked outright (in which case this tells us nothing).
function Get-LargestPingPayload {
    # If even a tiny ping fails, ICMP is filtered and every larger probe would "fail" too -
    # reporting that as a small MTU would be flatly wrong.
    & ping.exe -n 1 -w 3000 $HostName | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Log "Path MTU probe: host does not answer ping at all (ICMP filtered) - probe inconclusive, skipping."
        return $null
    }

    # 1472 payload + 28 bytes of IP/ICMP header = 1500, the standard Ethernet MTU.
    $low = 500
    $high = 1472
    $best = $null
    while ($low -le $high) {
        $mid = [int](($low + $high) / 2)
        & ping.exe -n 1 -w 3000 -f -l $mid $HostName | Out-Null
        if ($LASTEXITCODE -eq 0) {
            $best = $mid
            $low = $mid + 1
        } else {
            $high = $mid - 1
        }
    }
    return $best
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
    $smallOk = Invoke-UploadTest "TEST - small body" $smallPayload
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
$mainOk = Invoke-UploadTest "TEST - full-size body, one write (what the mod does today)" $mainPayload

$chunkedOk = $null
if ($AlsoTestChunkedSend) {
    $chunkedOk = Invoke-UploadTest "TEST - full-size body, sent in small pieces" $mainPayload $ChunkSizeBytes
}

$largestPing = $null
if ($AlsoProbePathMtu) {
    Write-Host ""
    Log "===== Path MTU probe (may take a few seconds) ====="
    $largestPing = Get-LargestPingPayload
    if ($largestPing -ne $null) {
        Log "Largest packet that reached the host: $largestPing byte payload = $($largestPing + 28) byte MTU."
        if ($largestPing -lt 1472) {
            Log "That is below the standard 1500 - the path really does have a smaller usable MTU."
        } else {
            Log "Full 1500-byte packets reach the host over ICMP."
        }
    }
}

Write-Host ""
Log "===== SUMMARY ====="
if ($AlsoTestSmallRequest) {
    Log "small body ($SmallPayloadSizeBytes bytes):        $(if ($smallOk) { 'got a response' } else { 'NO response' })"
}
Log "full body, one write ($($mainPayload.Length) bytes):  $(if ($mainOk) { 'got a response' } else { 'NO response' })"
if ($AlsoTestChunkedSend) {
    Log "full body, $ChunkSizeBytes-byte pieces:            $(if ($chunkedOk) { 'got a response' } else { 'NO response' })"
}
if ($largestPing -ne $null) {
    Log "largest packet reaching the host:      $($largestPing + 28) byte MTU"
}

Write-Host ""
if ($mainOk) {
    Log "=> Uploads work from this machine as-is. Nothing to fix here."
} elseif ($chunkedOk) {
    Log "=> IMPORTANT: the same body fails as one write but succeeds in small pieces."
    Log "   The path drops full-size packets. This is fixable in the mod by writing the upload"
    Log "   body in small chunks (HttpSendRequestEx + InternetWriteFile) instead of one big send."
} elseif ($smallOk) {
    Log "=> Size-dependent failure: small requests get through, large ones do not, and chunking"
    Log "   them did not help either. The path is dropping bulk upload traffic to this host."
    Log "   Worth trying on this machine: lower the network MTU, e.g. for a Wi-Fi adapter -"
    Log '     netsh interface ipv4 set subinterface "Wi-Fi" mtu=1400 store=persistent'
    Log '   (run as administrator; use "Ethernet" instead if on a cable). A VPN also usually'
    Log "   sidesteps it. Nothing the mod can change will help if even small packets cannot"
    Log "   carry the upload."
} else {
    Log "=> Nothing gets a response at all, regardless of size - the host/port is unreachable"
    Log "   from this machine."
}

Write-Host ""
Write-Host "Press Enter to close this window..."
Read-Host | Out-Null
