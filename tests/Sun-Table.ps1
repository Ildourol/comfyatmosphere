<#
.SYNOPSIS
  Turns the "sun measure" lines of comfyatmos.log into the table of the sun and the moons by the clock.

.DESCRIPTION
  tests\sun-table.json turns [sun] measure on, points the camera over the whole sky and steps the hour by half
  hours. Each measure line holds the game time and every sun-shaped quad the sky drew, as azimuth/elevation and the
  texture's width. The game clock runs on from each hour the test sets, so a line is put in the half hour it is
  within 3 minutes of; other lines are left out. In each half hour the quads of one texture are one body: the sun's
  texture is 128 wide, the larger moon's 512 and the smaller moon's 256, in this client.

  It prints each half hour's bodies, and with -Cpp the rows of kRows in src\sunpath.cpp: the mean hour of the
  half hour's lines, then each body's mean azimuth and elevation, and 1 where the sky drew it.

.PARAMETER Log
  The log to read. Default: the test client's Logs\comfyatmos.log.
#>
param(
    [string]$Log,
    [string]$Client = (Join-Path $env:USERPROFILE 'Desktop\wow-clients\octow - Copy'),
    [switch]$Cpp
)

$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
if (-not $Log) { $Log = Join-Path $Client 'Logs\comfyatmos.log' }
if (-not (Test-Path $Log)) { throw "No log at $Log" }

$bodyTex = @{ sun = 128; moon1 = 512; moon2 = 256 }
$lineRx = '^sun measure: hour ([\d.]+), \d+ quads?:(.*?) camera'
$quadRx = ' (-?[\d.]+)/(-?[\d.]+) tex (\d+);'

# Per half hour: the hours of its lines, and per texture the sum of the quads' unit vectors.
$bins = @{}
foreach ($l in [IO.File]::ReadLines($Log)) {
    $m = [regex]::Match($l, $lineRx)
    if (-not $m.Success) { continue }
    $h = [double]::Parse($m.Groups[1].Value, $inv)
    $key = [math]::Round($h * 2.0) / 2.0
    if ([math]::Abs($h - $key) -gt 0.05) { continue }
    $key = $key % 24.0
    if (-not $bins.ContainsKey($key)) { $bins[$key] = @{ hours = New-Object System.Collections.ArrayList; tex = @{} } }
    $bin = $bins[$key]
    # The hour against the half hour, so a line at 23:59 counts as 24:00 - 0.01, not 23.99 at 00:00.
    $null = $bin.hours.Add($key + ($h - [math]::Round($h * 2.0) / 2.0))
    foreach ($q in [regex]::Matches($m.Groups[2].Value, $quadRx)) {
        $az = [double]::Parse($q.Groups[1].Value, $inv) * [math]::PI / 180.0
        $el = [double]::Parse($q.Groups[2].Value, $inv) * [math]::PI / 180.0
        $t = [int]$q.Groups[3].Value
        if (-not $bin.tex.ContainsKey($t)) { $bin.tex[$t] = @{ x = 0.0; y = 0.0; z = 0.0; n = 0 } }
        $s = $bin.tex[$t]
        $s.x += [math]::Cos($el) * [math]::Cos($az)
        $s.y += [math]::Cos($el) * [math]::Sin($az)
        $s.z += [math]::Sin($el)
        $s.n++
    }
}
if ($bins.Count -eq 0) { throw "No 'sun measure' lines in $Log. Run tests\sun-table.json first." }

$rows = foreach ($key in (0..47 | ForEach-Object { $_ / 2.0 })) {
    $row = [ordered]@{ key = $key; hour = $key; n = 0 }
    if ($bins.ContainsKey($key)) {
        $row.hour = ($bins[$key].hours | Measure-Object -Average).Average
        $row.n = $bins[$key].hours.Count
    }
    foreach ($b in 'sun', 'moon1', 'moon2') {
        $s = if ($bins.ContainsKey($key)) { $bins[$key].tex[$bodyTex[$b]] } else { $null }
        if ($s) {
            $len = [math]::Sqrt($s.x * $s.x + $s.y * $s.y + $s.z * $s.z)
            $row[$b] = [pscustomobject]@{ az = [math]::Atan2($s.y, $s.x) * 180.0 / [math]::PI
                el = [math]::Asin([math]::Max(-1.0, [math]::Min(1.0, $s.z / $len))) * 180.0 / [math]::PI; n = $s.n }
        }
        else { $row[$b] = $null }
    }
    [pscustomobject]$row
}

$other = @($bins.Values | ForEach-Object { $_.tex.Keys } | Where-Object { $bodyTex.Values -notcontains $_ } | Sort-Object -Unique)
if ($other.Count) { Write-Warning "Quads with other textures, left out: $($other -join ', ')" }

foreach ($r in $rows) {
    $parts = foreach ($b in 'sun', 'moon1', 'moon2') {
        $v = $r.$b
        if ($v) { '{0,-5} {1,7:0.00}/{2,6:0.00} x{3,-3}' -f $b, $v.az, $v.el, $v.n } else { '{0,-5} {1,-18}' -f $b, '-' }
    }
    '{0:00}:{1:00}  {2}' -f [math]::Floor($r.key), (($r.key % 1) * 60), ($parts -join '  ')
}

if ($Cpp) {
    ''
    foreach ($r in $rows) {
        $cells = foreach ($b in 'sun', 'moon1', 'moon2') {
            $v = $r.$b
            if ($v) { '{{ {0,7:0.00}f, {1,6:0.00}f, true  }}' -f $v.az, $v.el } else { '{    0.00f,   0.00f, false }' }
        }
        '        {{ {0,7:0.0000}f, {1} }},' -f $r.hour, ($cells -join ', ')
    }
}
