<#
.SYNOPSIS
    Reproduces the mod's replay upload (UploadReplayBinary in src/Web/url_downloader.cpp) at the
    raw socket level and runs it through a matrix of variations, so upload failures can be
    diagnosed without rebuilding or redeploying the mod.

.DESCRIPTION
    The mod does, via WinINet:
        InternetConnect(host, port, INTERNET_SERVICE_HTTP, flags=0)
        HttpOpenRequest(POST, endpoint, INTERNET_FLAG_SECURE if UploadReplayDataUseTls else 0)
        HttpAddRequestHeaders("Content-Type: application/octet-stream")
        HttpSendRequest(..., 64KiB replay buffer)

    This script builds that HTTP/1.1 POST by hand over a raw socket (optionally TLS), so nothing
    a higher-level HTTP client does (redirects, retries, protocol upgrades) can mask what happens
    on the wire.

    WHAT IS ALREADY ESTABLISHED (so these are no longer the open questions):
      - A replay is always exactly 65536 bytes, and it is mostly empty: across 83 real replays,
        gzip gives 2285-5131 bytes, and merely dropping trailing zero bytes gives 6811-15379.
      - On the affected connection, ~1KB bodies reach the server but 64KiB never does. Sending it
        in 1000-byte pieces changes nothing, and the usable path MTU is 1486, so this is NOT a
        packet-size/MTU problem and cannot be fixed by writing the body differently.
      - A DPI-circumvention tool (zapret) being on or off makes no difference (~27KB vs ~25KB).
      - An unrelated control server accepts the same 64KiB body fine, so the machine itself can
        do large uploads.
      - Cloudflare does NOT redirect HTTP to HTTPS here: plain HTTP on port 80 is served directly.
      - Independent ISP testing shows the cutoff is the ISP's DPI throttling by provider: every
        Cloudflare (AS13335) endpoint dies at 16KB, while Fastly and Google Cloud are unaffected.
        Plain-HTTP endpoints consistently survive a bit longer than TLS ones (24KB vs 16-20KB).

    So this matrix exists to pin down the remaining choices: whether plain HTTP on port 80 buys a
    higher ceiling than TLS on 443, whether the body is being throttled (and would arrive given a
    much longer timeout) or outright dropped, whether talking to the origin directly avoids it,
    and confirmation that a gzip-sized body sails through.

    IMPORTANT when reading the output: any HTTP response at all counts as reaching the server -
    even 400, 405 or 500. Those mean the network carried the request and the server merely
    disliked the body (expected, since these are not valid replays). Only a timeout or connection
    error means the traffic never completed the round trip.

    HOW TO RUN THIS (no console/typing needed):
      1. Change the values in the "SETTINGS" block below if asked to (leave them alone otherwise).
      2. Save the file.
      3. Right-click the file -> "Run with PowerShell" (on some setups this is under a "Show
         more options" submenu on Windows 11).
      4. It prints a result matrix at the end and waits for a key press, so nothing disappears
         before it can be read or screenshotted.
         This takes SEVERAL MINUTES - every failing case has to burn its own timeout, and one
         case deliberately waits up to two minutes. Long pauses are normal, not a freeze.
#>

# ============================== SETTINGS ==============================
# Change these values if asked to, then save the file and re-run it.

$HostName          = "replays.blazqueue.com"  # the Cloudflare-fronted host the mod uploads to
$Endpoint          = "/upload"                # request path
$OriginIp          = "89.167.76.6"            # the server's own address, bypassing Cloudflare
$OriginPort        = 5000                     # and its port
$ControlHostName   = "postman-echo.com"       # unrelated server, as a control (must read the body)
$ControlEndpoint   = "/post"

$PayloadFilePath   = ""                       # optional: a real replay .dat, e.g. "D:\...\Save\Replay\archive\foo.dat"
                                              # leave blank ("") to use random bytes of the same size
$FullSizeBytes     = 65536                    # what the mod actually sends (64KiB)
$SmallSizeBytes    = 1024                     # a body small enough to be known-good
$GzipSizedBytes    = 5120                     # what a real replay compresses to (measured 2285-5131)

$RunLongTimeoutCase = $true                   # include the "is it throttled or dropped" case (waits up to $LongTimeoutSeconds)
$RunOriginCases     = $true                   # include the direct-to-origin cases
$RunControlCase     = $true                   # include the unrelated-control case
$RunSizeSweep       = $true                   # find the largest body that gets through, over HTTPS:443
$RunSplitCase       = $true                   # send the full body as several separate connections, each under the limit
$SplitChunkBytes    = 12000                   # size of each of those pieces (must stay under the ~16KB per-connection limit)
$GzipSizedTrials    = 3                       # repeat the gzip-sized case this many times, since results vary run to run
$TimeoutSeconds     = 30                      # normal per-case timeout
$LongTimeoutSeconds = 120                     # the patient case's timeout

# ========================================================================

function Log([string]$msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss.fff")
    Write-Host "[$ts] $msg"
}

function Format-Elapsed([System.Diagnostics.Stopwatch]$sw) {
    return "{0:N3}s" -f $sw.Elapsed.TotalSeconds
}

# Performs one POST and reports what happened. Returns an object rather than a bare bool so the
# matrix can say WHY something failed - "the OS gave up retransmitting" and "the server never
# answered in time" look identical in a pass/fail column but mean very different things.
function Invoke-UploadCase {
    param(
        [string]$label,
        [byte[]]$payload,
        [string]$targetHost,
        [int]$targetPort,
        [string]$targetEndpoint,
        [bool]$useTls,
        [int]$chunkSize = 0,
        [int]$timeoutSec = 0,
        [switch]$quiet
    )

    if ($timeoutSec -le 0) { $timeoutSec = $TimeoutSeconds }
    $scheme = if ($useTls) { "https" } else { "http" }

    if (-not $quiet) {
        Write-Host ""
        Log "===== $label ====="
        Log "  $scheme`://$targetHost`:$targetPort$targetEndpoint | body $($payload.Length) bytes | $(if ($chunkSize -gt 0) { "$chunkSize-byte pieces" } else { 'one write' }) | timeout ${timeoutSec}s"
    }

    $result = [pscustomobject]@{ Label = $label; Ok = $false; Status = ""; Reason = ""; Seconds = 0.0 }
    $overall = [System.Diagnostics.Stopwatch]::StartNew()

    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = $timeoutSec * 1000
    $client.SendTimeout = $timeoutSec * 1000
    if ($chunkSize -gt 0) {
        # Without this, TCP coalesces the small writes back into full-size segments, making the
        # chunked case indistinguishable from the single-write one.
        $client.NoDelay = $true
    }

    try {
        $connectSw = [System.Diagnostics.Stopwatch]::StartNew()
        # Task.Wait() throws (wrapped in an AggregateException) when the task faults - connection
        # refused, for instance - rather than returning false, so both paths need handling.
        try {
            $connectTask = $client.ConnectAsync($targetHost, $targetPort)
            if (-not $connectTask.Wait($timeoutSec * 1000)) {
                $result.Reason = "TCP connect timed out"
                if (-not $quiet) { Log "[error] $($result.Reason) after $(Format-Elapsed $connectSw)." }
                return $result
            }
        } catch {
            $inner = $_.Exception.InnerException
            $result.Reason = "TCP connect failed: $(if ($inner) { $inner.Message } else { $_.Exception.Message })"
            if (-not $quiet) { Log "[error] $($result.Reason) after $(Format-Elapsed $connectSw)." }
            return $result
        }
        if (-not $quiet) { Log "TCP connected in $(Format-Elapsed $connectSw)." }

        $stream = $client.GetStream()

        if ($useTls) {
            $tlsSw = [System.Diagnostics.Stopwatch]::StartNew()
            try {
                $sslStream = New-Object System.Net.Security.SslStream($stream, $false)
                $sslStream.AuthenticateAsClient($targetHost)
                $stream = $sslStream
                if (-not $quiet) { Log "TLS handshake completed in $(Format-Elapsed $tlsSw). Protocol: $($sslStream.SslProtocol)" }
            } catch {
                $result.Reason = "TLS handshake failed: $($_.Exception.Message)"
                if (-not $quiet) { Log "[error] $($result.Reason)" }
                return $result
            }
        }

        # Same request shape the mod builds.
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
            # This only means the bytes reached the local OS/TLS buffer. It does NOT mean they
            # reached the server: a body this size fits in local socket buffers and reports
            # success instantly even when every packet is dropped further along the path.
            if (-not $quiet) {
                Log "Request written ($($headerBytes.Length + $payload.Length) bytes) in $(Format-Elapsed $sendSw) - handed to the OS, delivery not yet confirmed."
            }
        } catch {
            $result.Reason = "write failed: $($_.Exception.Message)"
            if (-not $quiet) { Log "[error] $($result.Reason) after $(Format-Elapsed $sendSw)." }
            return $result
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

            $result.Ok = $true
            $result.Status = $statusLine
            $result.Reason = "reached the server"
            if ($quiet) {
                Log "  $($payload.Length) bytes -> $statusLine"
            } else {
                Log "Response received in $(Format-Elapsed $readSw)."
                Log "Status line: $statusLine"
                Log "Headers:"
                $responseHeaders | ForEach-Object { Log "`t$_" }
                # Truncated on purpose: an echo-style server sends the whole body back, which
                # would bury the matrix these results are read from.
                if ($body.Length -gt 300) {
                    Log "Body (first 300 of $($body.Length) chars): $($body.Substring(0, 300))"
                } else {
                    Log "Body: $body"
                }
                Log "RESULT: reached the server (any status counts - the network carried it)."
            }
            return $result
        } catch {
            # Distinguish the OS abandoning the connection from simply not answering in time.
            # A socket-level error well before the timeout means TCP exhausted its retransmits,
            # i.e. the data was being dropped, not merely delayed.
            $elapsed = $readSw.Elapsed.TotalSeconds
            if ($elapsed -lt ($timeoutSec - 2)) {
                $result.Reason = "connection died after $([int]$elapsed)s (OS gave up retransmitting - data was dropped)"
            } else {
                $result.Reason = "no answer within ${timeoutSec}s (connection alive, response never came)"
            }
            if ($quiet) {
                Log "  $($payload.Length) bytes -> NO response ($($result.Reason))"
            } else {
                Log "[error] $($result.Reason)"
                Log "RESULT: the request never completed a round trip."
            }
            return $result
        }
    } finally {
        $overall.Stop()
        $result.Seconds = [Math]::Round($overall.Elapsed.TotalSeconds, 1)
        $client.Close()
    }
}

# Binary-searches the largest body that still gets a response. Resolution is 512 bytes; the
# threshold is not a hard line (it moved between runs), so treat the number as approximate.
function Get-LargestWorkingBody {
    param([int]$knownGood, [int]$knownBad, [string]$targetHost, [int]$targetPort, [string]$targetEndpoint, [bool]$useTls)
    $low = $knownGood
    $high = $knownBad
    $best = $knownGood
    while ($low -le $high - 512) {
        $mid = [int](($low + $high) / 2)
        $probe = New-Object byte[] $mid
        (New-Object System.Random).NextBytes($probe)
        $r = Invoke-UploadCase -label "sweep" -payload $probe -targetHost $targetHost -targetPort $targetPort `
            -targetEndpoint $targetEndpoint -useTls $useTls -quiet
        if ($r.Ok) { $best = $mid; $low = $mid + 1 } else { $high = $mid - 1 }
    }
    return $best
}

# ---------------------------------------------------------------- payloads

if ($PayloadFilePath -ne "") {
    if (-not (Test-Path $PayloadFilePath)) {
        Log "[error] PayloadFilePath does not exist: '$PayloadFilePath'"
        Write-Host ""
        Write-Host "Press Enter to close this window..."
        Read-Host | Out-Null
        exit 1
    }
    $fullPayload = [System.IO.File]::ReadAllBytes($PayloadFilePath)
    Log "Full-size payload loaded from '$PayloadFilePath' ($($fullPayload.Length) bytes)."
} else {
    $fullPayload = New-Object byte[] $FullSizeBytes
    (New-Object System.Random).NextBytes($fullPayload)
}
$smallPayload = New-Object byte[] $SmallSizeBytes
(New-Object System.Random).NextBytes($smallPayload)
# Stands in for a gzipped replay. Deliberately NOT actually gzipped here: random bytes do not
# compress, so compressing the test payload would prove nothing about the real size on the wire.
$gzipSizedPayload = New-Object byte[] $GzipSizedBytes
(New-Object System.Random).NextBytes($gzipSizedPayload)

# ---------------------------------------------------------------- run the matrix

Log "Replay upload diagnosis matrix."
Log "  upload host: $HostName$Endpoint"
Log "  origin:      $OriginIp`:$OriginPort"
Log "  full body:   $($fullPayload.Length) bytes"

$results = @()

$results += Invoke-UploadCase -label "A. small body over HTTPS:443 (baseline: is the host reachable)" `
    -payload $smallPayload -targetHost $HostName -targetPort 443 -targetEndpoint $Endpoint -useTls $true

$results += Invoke-UploadCase -label "B. FULL body over HTTPS:443 (mod with UploadReplayDataUseTls=1)" `
    -payload $fullPayload -targetHost $HostName -targetPort 443 -targetEndpoint $Endpoint -useTls $true

$results += Invoke-UploadCase -label "C. FULL body over plain HTTP:80 (mod's native behavior)" `
    -payload $fullPayload -targetHost $HostName -targetPort 80 -targetEndpoint $Endpoint -useTls $false

if ($RunLongTimeoutCase) {
    $results += Invoke-UploadCase -label "D. FULL body over plain HTTP:80, waiting up to ${LongTimeoutSeconds}s (throttled, or dropped?)" `
        -payload $fullPayload -targetHost $HostName -targetPort 80 -targetEndpoint $Endpoint -useTls $false `
        -timeoutSec $LongTimeoutSeconds
}

$results += Invoke-UploadCase -label "E. gzip-sized body ($GzipSizedBytes bytes) over plain HTTP:80 (does compressing fix it)" `
    -payload $gzipSizedPayload -targetHost $HostName -targetPort 80 -targetEndpoint $Endpoint -useTls $false

# Repeated: this is the case a real fix would rely on, and it has flipped between runs, so one
# sample is not enough to call it reliable.
$gzipTlsOkCount = 0
for ($t = 1; $t -le $GzipSizedTrials; $t++) {
    $r = Invoke-UploadCase -label "F$t. gzip-sized body ($GzipSizedBytes bytes) over HTTPS:443 (try $t of $GzipSizedTrials)" `
        -payload $gzipSizedPayload -targetHost $HostName -targetPort 443 -targetEndpoint $Endpoint -useTls $true
    if ($t -eq 1) { $results += $r } else { $results += $r }
    if ($r.Ok) { $gzipTlsOkCount++ }
}

if ($RunOriginCases) {
    $results += Invoke-UploadCase -label "G. small body straight to the origin IP (is it reachable at all)" `
        -payload $smallPayload -targetHost $OriginIp -targetPort $OriginPort -targetEndpoint $Endpoint -useTls $false
    $results += Invoke-UploadCase -label "H. FULL body straight to the origin IP (does bypassing Cloudflare help)" `
        -payload $fullPayload -targetHost $OriginIp -targetPort $OriginPort -targetEndpoint $Endpoint -useTls $false
}

if ($RunControlCase) {
    $results += Invoke-UploadCase -label "I. FULL body to an unrelated control server (can this machine upload 64KiB at all)" `
        -payload $fullPayload -targetHost $ControlHostName -targetPort 443 -targetEndpoint $ControlEndpoint -useTls $true
}

$splitOk = $null
$splitDetail = ""
if ($RunSplitCase) {
    # The censorship this is up against counts data PER TCP CONNECTION, so the documented way
    # around it is to spread the payload over several connections that each stay under the limit.
    # If every piece lands, the mod could upload a replay in parts (with the server reassembling)
    # even without compressing it.
    Write-Host ""
    $pieces = [Math]::Ceiling($fullPayload.Length / $SplitChunkBytes)
    Log "===== J. FULL body split over $pieces separate connections of $SplitChunkBytes bytes each ====="
    $allOk = $true
    $okCount = 0
    for ($p = 0; $p -lt $pieces; $p++) {
        $off = $p * $SplitChunkBytes
        $len = [Math]::Min($SplitChunkBytes, $fullPayload.Length - $off)
        $piece = New-Object byte[] $len
        [Array]::Copy($fullPayload, $off, $piece, 0, $len)
        $r = Invoke-UploadCase -label "  piece $($p + 1)/$pieces" -payload $piece -targetHost $HostName `
            -targetPort 443 -targetEndpoint $Endpoint -useTls $true -quiet
        if ($r.Ok) { $okCount++ } else { $allOk = $false }
    }
    $splitOk = $allOk
    $splitDetail = "$okCount of $pieces pieces landed"
    Log "Split upload: $splitDetail."
}

$sweepTls = $null
if ($RunSizeSweep) {
    # Swept over HTTPS:443, not plain HTTP:80: field runs showed port 80 behaving erratically
    # (a 5KB body got through with a DPI-bypass tool running and was dropped without it), while
    # 443 has been consistent, which makes it the meaningful place to measure the ceiling.
    Write-Host ""
    Log "===== Size sweep over HTTPS:443 - largest body that gets through (a few minutes) ====="
    $sweepTls = Get-LargestWorkingBody -knownGood $SmallSizeBytes -knownBad $FullSizeBytes `
        -targetHost $HostName -targetPort 443 -targetEndpoint $Endpoint -useTls $true
    Log "Largest body over HTTPS:443: about $sweepTls bytes."
}

# ---------------------------------------------------------------- matrix

Write-Host ""
Log "===================== RESULTS ====================="
foreach ($r in $results) {
    $mark = if ($r.Ok) { "OK  " } else { "FAIL" }
    Log ("{0} | {1,5}s | {2}" -f $mark, $r.Seconds, $r.Label)
    Log ("       {0}{1}" -f $r.Reason, $(if ($r.Status) { " -> $($r.Status)" } else { "" }))
}
if ($splitOk -ne $null) {
    Log ("{0} | ------ | J. FULL body split over separate connections of {1} bytes ({2})" -f $(if ($splitOk) { "OK  " } else { "FAIL" }), $SplitChunkBytes, $splitDetail)
}
if ($sweepTls -ne $null) {
    Log ("     | ------ | largest single body over HTTPS:443: about {0} bytes" -f $sweepTls)
}

function Res([string]$prefix) { return ($results | Where-Object { $_.Label.StartsWith($prefix) } | Select-Object -First 1) }
$a = Res "A."; $b = Res "B."; $c = Res "C."; $d = Res "D."; $e = Res "E."; $g = Res "G."; $h = Res "H."; $i = Res "I."
# F is run several times; treat it as working only if every try worked, since a fix cannot rely
# on a case that only sometimes lands.
$f = [pscustomobject]@{ Ok = ($gzipTlsOkCount -eq $GzipSizedTrials) }

Write-Host ""
Log "===================== READING ====================="
if (-not $a.Ok) {
    Log "The host is not reachable from here at all, so nothing below is meaningful."
} elseif ($b.Ok -and $c.Ok) {
    Log "Full 64KiB uploads work from this machine. Nothing to fix here."
} else {
    Log "* gzip-sized body over HTTPS:443 landed $gzipTlsOkCount of $GzipSizedTrials tries."
    if ($f.Ok) {
        Log "  A gzip-sized body gets through reliably while the full 64KiB never does."
        Log "  Compressing the replay before upload would fix this: a replay is 64KiB of mostly"
        Log "  empty space and gzips to 2-5KB, clearing the limit with room to spare."
        Log "  This needs the server to accept a compressed body, so it is a coordinated change."
    } elseif ($gzipTlsOkCount -gt 0) {
        Log "  That is inconsistent, so compressing alone may not be dependable on this connection."
    }
    if ($splitOk -ne $null) {
        if ($splitOk) {
            Log "* Splitting the full body over separate connections of $SplitChunkBytes bytes DID land every piece."
            Log "  This matches how the censorship is documented to work - it counts data per TCP"
            Log "  connection - so uploading a replay in parts would also work, with no compression"
            Log "  needed. It does need the server to accept and reassemble the parts."
        } else {
            Log "* Splitting the full body over separate connections did NOT land every piece"
            Log "  ($splitDetail), so per-connection fragmentation alone is not a dependable fix here."
        }
    }
    if ($c.Ok -and -not $b.Ok) {
        Log "* Plain HTTP on port 80 works where HTTPS on 443 does not - so set"
        Log "  UploadReplayDataPort=80 and UploadReplayDataUseTls=0 as an immediate workaround."
    } elseif (-not $c.Ok -and -not $b.Ok) {
        Log "* Neither plain HTTP on 80 nor HTTPS on 443 carries the full body, so switching"
        Log "  scheme or port is not a way out on its own."
    }
    if ($d -ne $null) {
        if ($d.Ok) {
            Log "* The full body DID arrive when given ${LongTimeoutSeconds}s, so it is being throttled rather than"
            Log "  dropped - raising the mod's upload timeout would also make this work, just slowly."
        } else {
            Log "* Even with ${LongTimeoutSeconds}s the full body never arrived, so it is being dropped, not merely"
            Log "  slowed. Raising the mod's timeout would not help."
        }
    }
    if ($g -ne $null) {
        if ($g.Ok -and $h.Ok) {
            Log "* Talking straight to the origin IP works, including the full body - pointing"
            Log "  UploadReplayDataHost at the origin and skipping Cloudflare is a workaround here."
        } elseif ($g.Ok -and -not $h.Ok) {
            Log "* The origin IP is reachable but still refuses the full body, so Cloudflare is not"
            Log "  what imposes the limit - the same ceiling applies without it."
        } elseif (-not $g.Ok) {
            Log "* The origin IP is not reachable at all from here, which is why it is fronted by"
            Log "  Cloudflare in the first place - going direct is not an option."
        }
    }
    if ($i -ne $null -and $i.Ok) {
        Log "* An unrelated server accepted the same 64KiB body, so this machine can upload large"
        Log "  bodies in general. The limit is tied to the route to this particular host."
        Log "  Independent testing shows this ISP throttling by hosting provider - every"
        Log "  Cloudflare endpoint dies around 16KB while Fastly and Google Cloud are unaffected -"
        Log "  so moving the replay server behind one of those would also sidestep it."
    }
}

Write-Host ""
Write-Host "Press Enter to close this window..."
Read-Host | Out-Null
