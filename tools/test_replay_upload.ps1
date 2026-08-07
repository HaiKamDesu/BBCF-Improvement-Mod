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

    This script opens a raw TCP socket (optionally wrapped in TLS) and writes a hand-built
    HTTP/1.1 POST byte-for-byte, so it fails/succeeds under the same conditions WinINet would -
    no higher-level HTTP client behavior (redirects, retries, protocol upgrades) to mask what is
    actually happening on the wire.

    It runs several rounds and prints a summary that interprets them:
      - a small body, to prove the host is reachable at all
      - the full 64KiB body in one write, which is exactly what the mod does
      - the full body in small pieces, to tell an MTU/packet-size problem apart from a
        bulk-volume one (if only this form survives, the mod can be fixed by switching to
        HttpSendRequestEx + InternetWriteFile)
      - a size sweep, to find the largest body that still gets through
      - the same small/large pair against an unrelated control server, to tell "this machine
        cannot do large uploads to anything" apart from "something on the path to THIS host
        breaks them"
      - a best-effort path-MTU ping probe

    IMPORTANT when reading the output: any HTTP response at all counts as reaching the server -
    even 400, 405 or 500. Those mean the network path works and the server merely disliked the
    body (expected for random test payloads, which are not valid replays). Only a timeout or
    connection error means the traffic never completed the round trip.

    If a DPI-circumvention tool (zapret, GoodbyeDPI, ByeDPI, ...) or a VPN is running, note it
    when reporting results, and ideally run the script both with it on and with it off. Those
    tools work by actively rewriting outbound packets, which can break sustained uploads while
    leaving small requests and handshakes working perfectly.

    HOW TO RUN THIS (no console/typing needed):
      1. Change the values in the "SETTINGS" block below if asked to (leave them alone otherwise).
      2. Save the file.
      3. Right-click the file -> "Run with PowerShell" (on some setups this is under a "Show
         more options" submenu on Windows 11).
      4. A black window will open, run the tests, print a summary, and wait for a key press
         before closing - so nothing disappears before it can be read/screenshotted.
         Expect it to take a few minutes: every failing round has to sit through its own
         ~19 second timeout, so long pauses are normal and not a freeze.
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
$SmallPayloadSizeBytes = 1024                     # size of the small "is the host reachable at all" body
$AlsoTestChunkedSend   = $true                    # also send the full body in small pieces
$ChunkSizeBytes        = 1000                     # size of each piece for that chunked test
$AlsoProbeBodySizeLimit = $true                   # sweep for the largest body that still gets a response
$AlsoTestControlHost   = $true                    # repeat small+large against an unrelated server, as a control
$ControlHostName       = "postman-echo.com"       # that control server (must actually read the body it is sent)
$ControlPort           = 443
$ControlEndpoint       = "/post"
$AlsoProbePathMtu      = $true                    # best-effort ping probe for the largest packet that reaches the host
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
# pieces that size, with Nagle disabled so each piece leaves as its own small TCP packet.
# $quiet suppresses the response dump, for the size sweep where only pass/fail matters.
function Invoke-UploadTest {
    param(
        [string]$label,
        [byte[]]$payload,
        [int]$chunkSize = 0,
        [string]$targetHost = $HostName,
        [int]$targetPort = $Port,
        [string]$targetEndpoint = $Endpoint,
        [bool]$targetTls = $UseTls,
        [switch]$quiet
    )

    if (-not $quiet) {
        Write-Host ""
        Log "===== $label : $($payload.Length) byte body, host: $targetHost, tls: $targetTls, chunk: $(if ($chunkSize -gt 0) { "$chunkSize bytes" } else { 'one write' }) ====="
    }

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
            $connectTask = $client.ConnectAsync($targetHost, $targetPort)
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
        if (-not $quiet) { Log "TCP connected in $(Format-Elapsed $connectSw)." }

        $stream = $client.GetStream()

        if ($targetTls) {
            $tlsSw = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $sslStream = New-Object System.Net.Security.SslStream($stream, $false)
                $sslStream.AuthenticateAsClient($targetHost)
                $stream = $sslStream
                if (-not $quiet) { Log "TLS handshake completed in $(Format-Elapsed $tlsSw). Protocol: $($sslStream.SslProtocol)" }
            } catch {
                Log "[error] TLS handshake failed after $(Format-Elapsed $tlsSw). $($_.Exception.Message)"
                return $false
            }
        }

        # Same request shape the mod builds: POST <endpoint>, Content-Type: application/octet-stream.
        $headerText = @(
            "POST $targetEndpoint HTTP/1.1",
            "Host: $targetHost",
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
            if (-not $quiet) {
                Log "Request written ($($headerBytes.Length + $payload.Length) bytes) in $(Format-Elapsed $sendSw) - handed to the OS, delivery not yet confirmed."
            }
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

            if ($quiet) {
                Log "  $($payload.Length) bytes -> response: $statusLine"
            } else {
                Log "Response received in $(Format-Elapsed $readSw)."
                Log "Status line: $statusLine"
                Log "Headers:"
                $responseHeaders | ForEach-Object { Log "`t$_" }
                # Truncated on purpose: an echo-style server sends the whole 64KiB body back, and
                # dumping that buries the summary these results are read from.
                if ($body.Length -gt 400) {
                    Log "Body (first 400 of $($body.Length) chars): $($body.Substring(0, 400))"
                } else {
                    Log "Body: $body"
                }
                Log "RESULT: reached the server and got a response (any status counts as the network working)."
            }
            return $true
        } catch {
            if ($quiet) {
                Log "  $($payload.Length) bytes -> NO response after $(Format-Elapsed $readSw)"
            } else {
                Log "[error] No response. Failed after $(Format-Elapsed $readSw). $($_.Exception.Message)"
                Log "RESULT: the request never completed a round trip."
            }
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

# Binary-searches the largest body that still gets a response, between a known-good and a
# known-bad size. Knowing the cutoff says whether shrinking the upload (e.g. compressing the
# replay before sending) could realistically get under it.
function Get-LargestWorkingBody([int]$knownGood, [int]$knownBad) {
    $low = $knownGood
    $high = $knownBad
    $best = $knownGood
    while ($low -le $high - 512) {
        $mid = [int](($low + $high) / 2)
        $probe = New-Object byte[] $mid
        (New-Object System.Random).NextBytes($probe)
        if (Invoke-UploadTest -label "sweep" -payload $probe -quiet) {
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

$smallPayload = New-Object byte[] $SmallPayloadSizeBytes
(New-Object System.Random).NextBytes($smallPayload)
$smallOk = Invoke-UploadTest -label "TEST - small body" -payload $smallPayload

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
$mainOk = Invoke-UploadTest -label "TEST - full-size body, one write (what the mod does today)" -payload $mainPayload

$chunkedOk = $null
if ($AlsoTestChunkedSend -and -not $mainOk) {
    $chunkedOk = Invoke-UploadTest -label "TEST - full-size body, sent in small pieces" -payload $mainPayload -chunkSize $ChunkSizeBytes
}

$sizeLimit = $null
if ($AlsoProbeBodySizeLimit -and $smallOk -and -not $mainOk) {
    Write-Host ""
    Log "===== Size sweep: largest body that still gets a response (this takes a few minutes) ====="
    $sizeLimit = Get-LargestWorkingBody $smallPayload.Length $mainPayload.Length
    Log "Largest body that got a response: about $sizeLimit bytes."
}

$controlSmallOk = $null
$controlLargeOk = $null
if ($AlsoTestControlHost) {
    Write-Host ""
    Log "===== Control server ($ControlHostName): is it this host, or every large upload? ====="
    $controlSmallOk = Invoke-UploadTest -label "CONTROL - small body" -payload $smallPayload `
        -targetHost $ControlHostName -targetPort $ControlPort -targetEndpoint $ControlEndpoint -targetTls $true
    $controlLargeOk = Invoke-UploadTest -label "CONTROL - full-size body" -payload $mainPayload `
        -targetHost $ControlHostName -targetPort $ControlPort -targetEndpoint $ControlEndpoint -targetTls $true
}

$largestPing = $null
if ($AlsoProbePathMtu) {
    Write-Host ""
    Log "===== Path MTU probe (may take a few seconds) ====="
    $largestPing = Get-LargestPingPayload
    if ($largestPing -ne $null) {
        Log "Largest packet that reached the host: $largestPing byte payload = $($largestPing + 28) byte MTU."
    }
}

Write-Host ""
Log "===== SUMMARY ====="
Log "$HostName - small body ($($smallPayload.Length) bytes):   $(if ($smallOk) { 'got a response' } else { 'NO response' })"
Log "$HostName - full body ($($mainPayload.Length) bytes), one write: $(if ($mainOk) { 'got a response' } else { 'NO response' })"
if ($chunkedOk -ne $null) {
    Log "$HostName - full body in $ChunkSizeBytes-byte pieces:  $(if ($chunkedOk) { 'got a response' } else { 'NO response' })"
}
if ($sizeLimit -ne $null) {
    Log "$HostName - largest body that worked:      about $sizeLimit bytes"
}
if ($controlSmallOk -ne $null) {
    Log "$ControlHostName - small body:            $(if ($controlSmallOk) { 'got a response' } else { 'NO response' })"
    Log "$ControlHostName - full body:             $(if ($controlLargeOk) { 'got a response' } else { 'NO response' })"
}
if ($largestPing -ne $null) {
    Log "largest packet reaching $HostName`:       $($largestPing + 28) byte MTU"
}

Write-Host ""
if ($mainOk) {
    Log "=> Uploads work from this machine as-is. Nothing to fix here."
} elseif ($chunkedOk) {
    Log "=> The same body fails as one write but succeeds in small pieces, so the path drops"
    Log "   full-size packets. This is fixable in the mod, by writing the upload body in small"
    Log "   chunks (HttpSendRequestEx + InternetWriteFile) instead of one big send."
} elseif (-not $smallOk) {
    Log "=> Nothing gets a response at all, regardless of size - the host/port is unreachable"
    Log "   from this machine."
} else {
    Log "=> Large uploads do not complete, and sending them in small pieces did not help either,"
    Log "   so this is not a packet-size/MTU problem - something is dropping the upload once"
    Log "   enough data flows."
    if ($controlSmallOk -and $controlLargeOk) {
        Log "   The control server DID accept the same large body, so this machine can do large"
        Log "   uploads in general - it is specific to the path to $HostName."
    } elseif ($controlSmallOk -and -not $controlLargeOk) {
        Log "   The control server ALSO refused the large body while accepting the small one, so"
        Log "   large uploads are broken from this machine generally, not just to $HostName."
        Log "   That points at this machine's own network: an ISP/DPI filter, or a"
        Log "   DPI-circumvention tool (zapret, GoodbyeDPI, ByeDPI) rewriting outbound packets."
        Log "   Try turning any such tool OFF and re-running, and try again over a VPN."
    }
    Log "   No change inside the mod can help with this: the bytes are not reaching the server"
    Log "   no matter how they are written."
}

Write-Host ""
Write-Host "Press Enter to close this window..."
Read-Host | Out-Null
