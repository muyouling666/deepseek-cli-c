# build-curl-mingw.ps1 - compile a static libcurl (schannel TLS backend)
# directly with MinGW-w64 gcc, without autotools.
#
# On Windows, libcurl's lib/curl_setup.h picks up the hand written
# lib/config-win32.h when HAVE_CONFIG_H is *not* defined, so no configure step
# is needed. This is the fallback used when no POSIX shell is available to run
# ./configure (Windows without Git Bash / MSYS2).
#
# Usage:
#   pwsh -File scripts/build-curl-mingw.ps1 -SourceDir <curl-src> `
#        -OutDir build/curl-mingw -Gcc C:\mingw64\bin\gcc.exe [-Jobs 16]
param(
    [Parameter(Mandatory = $true)][string]$SourceDir,
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Gcc,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$libDir = Join-Path $SourceDir 'lib'
if (-not (Test-Path (Join-Path $libDir 'curl_setup.h'))) {
    throw "not a libcurl source tree: $SourceDir"
}
if (-not (Test-Path $Gcc)) {
    throw "gcc not found: $Gcc"
}

$objDir = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

# Everything under lib/ plus the vauth/vtls/vquic/vssh subdirectories, minus
# the TLS and SSH backends we are not building.
$drop = @(
    'curl_openssl.c', 'curl_gnutls.c', 'curl_nss.c', 'curl_mbedtls.c',
    'curl_wolfssl.c', 'curl_rustls.c', 'curl_bearssl.c', 'curl_sectransp.c',
    'curl_amissl.c', 'curl_mesalink.c',
    'libssh.c', 'libssh2.c', 'wolfssh.c',
    'curl_ngtcp2.c', 'curl_quiche.c', 'curl_msh3.c', 'curl_osslq.c'
)

$sources = @(Get-ChildItem -Path $libDir -Filter '*.c' -File)
foreach ($sub in @('vauth', 'vtls', 'vquic', 'vssh')) {
    $d = Join-Path $libDir $sub
    if (Test-Path $d) { $sources += Get-ChildItem -Path $d -Filter '*.c' -File }
}
$sources = $sources | Where-Object { $drop -notcontains $_.Name }

$defines = @(
    '-DBUILDING_LIBCURL',
    '-DCURL_STATICLIB',
    '-DUSE_SCHANNEL',
    '-DUSE_WINDOWS_SSPI',
    '-D_WIN32_WINNT=0x0601',
    '-DCURL_DISABLE_LDAP',
    '-DCURL_DISABLE_LDAPS',
    '-DCURL_DISABLE_FTP',
    '-DCURL_DISABLE_FILE',
    '-DCURL_DISABLE_TELNET',
    '-DCURL_DISABLE_DICT',
    '-DCURL_DISABLE_TFTP',
    '-DCURL_DISABLE_RTSP',
    '-DCURL_DISABLE_POP3',
    '-DCURL_DISABLE_IMAP',
    '-DCURL_DISABLE_SMTP',
    '-DCURL_DISABLE_GOPHER',
    '-DCURL_DISABLE_MQTT',
    '-DCURL_DISABLE_SMB'
)

$includes = @("-I$SourceDir/include", "-I$libDir")

Write-Host "compiling $($sources.Count) libcurl sources ($Jobs parallel)"

$worker = {
    param($src, $obj, $inc, $def, $gccPath)
    $output = & $gccPath -c -O2 -w @def @inc -o $obj $src 2>&1
    [pscustomobject]@{
        Source = $src
        Object = $obj
        Exit   = $LASTEXITCODE
        Output = ($output | Out-String)
    }
}

$queue = [System.Collections.Queue]::new()
foreach ($f in $sources) {
    $queue.Enqueue([pscustomobject]@{
        Source = $f.FullName
        Object = Join-Path $objDir ($f.BaseName + '.o')
    })
}

$running = @()
$failures = @()
$done = 0

while ($queue.Count -gt 0 -or $running.Count -gt 0) {
    while ($running.Count -lt $Jobs -and $queue.Count -gt 0) {
        $item = $queue.Dequeue()
        $running += Start-Job -ScriptBlock $worker -ArgumentList $item.Source, $item.Object, $includes, $defines, $Gcc
    }
    Start-Sleep -Milliseconds 150
    foreach ($j in @($running)) {
        if ($j.State -in @('Completed', 'Failed', 'Stopped')) {
            $res = Receive-Job $j
            Remove-Job $j -Force
            $running = @($running | Where-Object { $_.Id -ne $j.Id })
            $done++
            if ($j.State -ne 'Completed' -or ($res -and $res.Exit -ne 0)) {
                $name = if ($res) { Split-Path $res.Source -Leaf } else { "job $($j.Id)" }
                $failures += "$name :: $($res.Output)"
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "FAILED to compile $($failures.Count) source(s):"
    $failures | Select-Object -First 5 | ForEach-Object { Write-Host $_ }
    exit 1
}

$objs = @(Get-ChildItem -Path $objDir -Filter '*.o' -File)
Write-Host "compiled $($objs.Count) objects"

$ar = Join-Path (Split-Path $Gcc) 'ar.exe'
$libOut = Join-Path $OutDir 'libcurl.a'
if (Test-Path $libOut) { Remove-Item $libOut -Force }
& $ar rcs $libOut $objs.FullName
if ($LASTEXITCODE -ne 0) { throw "ar failed with exit code $LASTEXITCODE" }

Write-Host "built $libOut ($([math]::Round((Get-Item $libOut).Length / 1MB, 1)) MB)"

$incOut = Join-Path $OutDir 'include\curl'
New-Item -ItemType Directory -Force -Path $incOut | Out-Null
Copy-Item -Path (Join-Path $SourceDir 'include\curl\*.h') -Destination $incOut -Force
Write-Host "headers -> $incOut"
