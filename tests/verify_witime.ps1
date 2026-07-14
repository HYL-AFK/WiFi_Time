$ErrorActionPreference = 'Stop'

function Assert-True([bool]$condition, [string]$message) {
    if (-not $condition) { throw "FAIL: $message" }
}

function Matrix-Index([int]$x, [int]$y) {
    Assert-True ($x -ge 0 -and $x -lt 16 -and $y -ge 0 -and $y -lt 16) "matrix coordinate out of range"
    return ($y * 16 + $(if (($y % 2) -eq 1) { 15 - $x } else { $x }))
}

Assert-True ((Matrix-Index 0 0) -eq 0) "row 0 starts at LED 0"
Assert-True ((Matrix-Index 15 0) -eq 15) "row 0 ends at LED 15"
Assert-True ((Matrix-Index 15 1) -eq 16) "row 1 reverses at LED 16"
Assert-True ((Matrix-Index 0 1) -eq 31) "row 1 ends at LED 31"
Assert-True ((Matrix-Index 0 15) -eq 255) "last row stays in bounds"

$required = @(
    'main/drivers/ds3231.c', 'main/drivers/ds3231.h',
    'main/drivers/ws2812_matrix.c', 'main/drivers/ws2812_matrix.h',
    'main/APP/app_rtc.c', 'main/APP/app_time.c',
    'main/APP/app_weather.c', 'main/APP/app_display.c',
    'D:\Project\WiTime_Hardware\witime_design.drawio'
)
foreach ($path in $required) { Assert-True (Test-Path $path) "missing $path" }
$pinoutPath = Join-Path 'D:\Project\WiTime_Hardware' (([char]0x5f15) + ([char]0x811a) + ([char]0x56fe) + '.drawio')
Assert-True (Test-Path $pinoutPath) "missing user pinout file"

$rtcSource = Get-Content -Raw main/APP/app_rtc.c
$displaySource = Get-Content -Raw main/APP/app_display.c
$timeSource = Get-Content -Raw main/APP/app_time.c
$weatherSource = Get-Content -Raw main/APP/app_weather.c
$timeSnapshotHeader = Get-Content -Raw main/common/time_snapshot.h
$ds3231Source = Get-Content -Raw main/drivers/ds3231.c
$ds3231Header = Get-Content -Raw main/drivers/ds3231.h

Assert-True (-not $rtcSource.Contains('value.valid')) "RTC task must not validate RTC time"
Assert-True (-not $displaySource.Contains('t.valid')) "display task must not gate RTC display on valid"
Assert-True (-not $timeSource.Contains('.valid=true')) "SNTP snapshot must not use removed time valid flag"
Assert-True (-not $timeSnapshotHeader.Contains('bool valid')) "time snapshot must not expose RTC validity"
Assert-True (-not $ds3231Source.Contains('out->valid')) "DS3231 driver must not calculate time validity"
$timeResult = [regex]::Match($ds3231Header, '(?s)typedef struct \{.*?\} ds3231_time_t;').Value
Assert-True ($timeResult.Length -gt 0) "missing DS3231 time result type"
Assert-True (-not $timeResult.Contains('bool valid;')) "DS3231 time result must not expose time validity"

Assert-True ($displaySource.Contains('#define CLOCK_W 13')) "missing 13-column clock layout"
Assert-True ($displaySource.Contains('#define CLOCK_H 5')) "missing 5-row clock layout"
Assert-True ($displaySource.Contains('#define SECONDS_X 15')) "missing rightmost seconds column"
Assert-True ($displaySource.Contains('tm_min') -and $displaySource.Contains('30')) "missing half-hour animation trigger"
Assert-True ($displaySource.Contains('FLOW_FRAME') -or $displaySource.Contains('flow_animation')) "missing full-screen flow animation"
Assert-True ($weatherSource.Contains('pdMS_TO_TICKS(600000)')) "weather task must refresh every 10 minutes"

$drawio = New-Object System.Xml.XmlDocument
$drawio.Load('D:\Project\WiTime_Hardware\witime_design.drawio')
$diagrams = @($drawio.mxfile.diagram)
Assert-True ($diagrams.Count -eq 3) "draw.io must contain three diagrams"

$ids = @($drawio.SelectNodes('//mxCell') | ForEach-Object { $_.id })
$duplicateIds = @($ids | Group-Object | Where-Object Count -gt 1)
Assert-True ($duplicateIds.Count -eq 0) "draw.io node ids must be unique"
Assert-True (-not $drawio.SelectSingleNode('//mxCell[@id="a4"]')) "flow must not contain RTC validity decision"
Assert-True (-not $drawio.SelectSingleNode('//mxCell[@id="a6"]')) "flow must not contain RTC invalid branch"
$rtcSnapshotNode = $drawio.SelectSingleNode('//mxCell[@id="a5"]')
$rtcSnapshotEdge = $drawio.SelectSingleNode('//mxCell[@id="x03"]')
Assert-True ($rtcSnapshotNode -and $rtcSnapshotNode.value.Contains('from_rtc')) "flow must contain direct RTC snapshot write"
Assert-True ($rtcSnapshotEdge.source -eq 'a3' -and $rtcSnapshotEdge.target -eq 'a5') "RTC flow must be direct"
Assert-True ($drawio.OuterXml.Contains('GPIO8')) "missing GPIO8 mapping"
Assert-True ($drawio.OuterXml.Contains('GPIO10')) "missing GPIO10 mapping"
Assert-True ((Get-Content -Raw main/drivers/ws2812_matrix.c).Contains('serpentine')) "missing serpentine implementation"

$matrixPage = @($drawio.mxfile.diagram | Where-Object { $_.id -eq 'matrix-page' })
Assert-True ($matrixPage.Count -eq 1) "missing matrix page"
$matrixRoot = $matrixPage[0].mxGraphModel.root
$matrixClock = $matrixRoot.SelectSingleNode('mxCell[@id="mclock"]')
$matrixFill = $matrixRoot.SelectSingleNode('mxCell[@id="mfill"]')
$matrixSeconds = $matrixRoot.SelectSingleNode('mxCell[@id="mseconds"]')
$matrixPeriod = $matrixRoot.SelectSingleNode('mxCell[@id="mperiod"]')
Assert-True ($matrixClock.value.Contains('13') -and $matrixClock.value.Contains('x=0..12') -and $matrixClock.value.Contains('y=0..4')) "matrix clock must be 13x5"
Assert-True ($matrixFill.value.Contains('13') -and $matrixFill.value.Contains('x=0..12') -and $matrixFill.value.Contains('y=6..9')) "matrix fill area is incorrect"
Assert-True ($matrixSeconds.value.Contains('x=15') -and $matrixSeconds.value.Contains('y=0..14') -and $matrixSeconds.value.Contains('y=15')) "matrix seconds column is incorrect"
Assert-True ($matrixPeriod -ne $null) "matrix page must document periodic behavior"
$timezoneLabel = ([char]0x65f6) + ([char]0x533a)
Assert-True (-not $matrixPage[0].OuterXml.Contains($timezoneLabel)) "matrix page must not display timezone"

$pinout = New-Object System.Xml.XmlDocument
$pinout.Load($pinoutPath)
Assert-True ($pinout.OuterXml.Contains('GPIO8')) "missing DS3231 SDA mapping"
Assert-True ($pinout.OuterXml.Contains('GPIO9')) "missing DS3231 SCL mapping"
Assert-True ($pinout.OuterXml.Contains('GPIO10')) "missing WS2812 mapping"

Write-Output 'PASS: WiTime static verification'
