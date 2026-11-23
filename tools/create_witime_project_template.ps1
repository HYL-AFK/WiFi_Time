param(
    [string]$OutputPath = "outputs/witime_project_template_20260730/WiTime_项目开发资料填写模板.xlsx",
    [switch]$SingleSheet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Escape-Xml([string]$Value) {
    if ($null -eq $Value) { return '' }
    return [System.Security.SecurityElement]::Escape($Value).Replace("`r`n", "&#10;").Replace("`n", "&#10;")
}

function Col([int]$Index) {
    $name = ''
    while ($Index -gt 0) { $Index--; $name = [char](65 + ($Index % 26)) + $name; $Index = [math]::Floor($Index / 26) }
    return $name
}

function Cell([int]$Row, [int]$Column, [string]$Value, [int]$Style = 0, [string]$Formula = $null) {
    $ref = "$(Col $Column)$Row"
    if (-not [string]::IsNullOrEmpty($Formula)) { return '<c r="' + $ref + '" s="' + $Style + '"><f>' + $Formula + '</f></c>' }
    if ($null -eq $Value -or $Value -eq '') { return '<c r="' + $ref + '" s="' + $Style + '"/>' }
    return '<c r="' + $ref + '" s="' + $Style + '" t="inlineStr"><is><t xml:space="preserve">' + (Escape-Xml $Value) + '</t></is></c>'
}

function Build-Sheet([string]$Name, [hashtable]$Cells, [array]$Merges = @(), [hashtable]$Widths = @{}, [hashtable]$Heights = @{}, [array]$Validations = @()) {
    $rows = @{}
    foreach ($key in $Cells.Keys) {
        $parts = $key -split ','; $r = [int]$parts[0]; $c = [int]$parts[1]
        if (-not $rows.ContainsKey($r)) { $rows[$r] = @{} }
        $entry = $Cells[$key]
        $rows[$r][$c] = Cell -Row $r -Column $c -Value $entry['v'] -Style $entry['s'] -Formula $entry['f']
    }
    $rowXml = foreach ($r in ($rows.Keys | Sort-Object)) {
        $height = if ($Heights.ContainsKey($r)) { ' ht="' + $Heights[$r] + '" customHeight="1"' } else { '' }
        $cellsInOrder = foreach ($c in ($rows[$r].Keys | Sort-Object {[int]$_})) { $rows[$r][$c] }
        '<row r="' + $r + '"' + $height + '>' + ($cellsInOrder -join '') + '</row>'
    }
    $colXml = foreach ($c in ($Widths.Keys | Sort-Object {[int]$_})) { '<col min="' + $c + '" max="' + $c + '" width="' + $Widths[$c] + '" customWidth="1"/>' }
    $mergeXml = if ($Merges.Count) { '<mergeCells count="' + $Merges.Count + '">' + (($Merges | ForEach-Object { '<mergeCell ref="' + $_ + '"/>' }) -join '') + '</mergeCells>' } else { '' }
    $validationXml = if ($Validations.Count) { '<dataValidations count="' + $Validations.Count + '">' + (($Validations | ForEach-Object { '<dataValidation type="list" allowBlank="1" showInputMessage="1" showErrorMessage="1" sqref="' + $_.r + '"><formula1>&quot;' + $_.v + '&quot;</formula1></dataValidation>' }) -join '') + '</dataValidations>' } else { '' }
    return @"
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheetViews><sheetView workbookViewId="0" showGridLines="0"/></sheetViews>
  <sheetFormatPr defaultRowHeight="18"/>
  <cols>$($colXml -join '')</cols>
  <sheetData>$($rowXml -join '')</sheetData>
  $mergeXml
  $validationXml
  <pageMargins left="0.3" right="0.3" top="0.5" bottom="0.5" header="0.2" footer="0.2"/>
  <pageSetup orientation="landscape" paperSize="9" fitToWidth="1" fitToHeight="0"/>
</worksheet>
"@
}

function V([string]$Value, [int]$Style = 0, [string]$Formula = $null) { return @{ v = $Value; s = $Style; f = $Formula } }
function Add-Row([hashtable]$Map, [int]$Row, [string[]]$Values, [int[]]$Styles = $null) {
    for ($i = 0; $i -lt $Values.Count; $i++) { $Map["$Row,$($i+1)"] = V $Values[$i] $(if ($null -eq $Styles) { 0 } else { $Styles[$i] }) }
}

$title = 1; $section = 2; $header = 3; $label = 4; $known = 5; $input = 6; $center = 7; $formula = 8; $note = 9; $grid = 11
$mainText = 12; $mainLabel = 13; $mainGrid = 14
$thin = 10
$stdWidths = @{1=5;2=16;3=20;4=18;5=20;6=18;7=16;8=16;9=16;10=16}
$sheets = @()

# 00 填写说明
$c=@{}
$c['1,1']=V 'WiTime ESP32-S3 Wi-Fi 时钟 - 项目开发资料填写模板' $title
$c['3,1']=V '使用说明' $section
Add-Row $c 4 @('编号','填写要求','说明') @($header,$header,$header)
Add-Row $c 5 @('1','黄色单元格为待填写项','根据样机、采购单、测试记录填写；不要用估算值替代实测值。') @($center,$input,$note)
Add-Row $c 6 @('2','浅绿色单元格为代码/README已确认信息','来源于项目当前代码，若硬件实物不一致，应保留原记录并在变更表登记。') @($center,$known,$note)
Add-Row $c 7 @('3','状态字段使用下拉值','未开始、进行中、已完成、阻塞、待确认；验收后再改为已完成。') @($center,$input,$note)
Add-Row $c 8 @('4','资料来源需可追溯','填写文件名、版本、日期或链接；照片和图纸建议保存在项目资料目录。') @($center,$input,$note)
$c['10,1']=V '当前已确认的项目事实' $section
Add-Row $c 11 @('项目项','已确认内容','证据位置') @($header,$header,$header)
Add-Row $c 12 @('主控','ESP32-S3，ESP-IDF/FreeRTOS','README.md；main/CMakeLists.txt') @($label,$known,$note)
Add-Row $c 13 @('显示','WS2812 16×16 点阵，共 256 像素，GPIO10','README.md；main/drivers/ws2812_matrix.c') @($label,$known,$note)
Add-Row $c 14 @('时钟','DS3231，I2C0，SDA GPIO8、SCL GPIO9、地址 0x68','README.md；main/drivers/ds3231.c') @($label,$known,$note)
Add-Row $c 15 @('联网','Wi-Fi APSTA + BluFi 配网；SNTP 校时；Open-Meteo 天气','README.md；main/middle/ble_provision.c') @($label,$known,$note)
Add-Row $c 16 @('感应','LD2410C：OUT GPIO13；UART1 TX GPIO1 / RX GPIO2，256000bps','main/drivers/radar_presence.c') @($label,$known,$note)
$sheets += @{name='00 填写说明'; xml=(Build-Sheet '00 填写说明' $c @('A1:J1','A3:C3','A10:C10') @{1=7;2=20;3=72;4=8;5=8;6=8;7=8;8=8;9=8;10=8} @{1=34;3=24;4=26;5=36;6=36;7=28;8=34;10=24;11=24;12=25;13=25;14=25;15=25;16=25})}

# A 产品开发总表（严格按领导截图的白底分格格式填写）
$c=@{}
$c['1,1']=V '基于 ESP32-S3 的点阵屏天气时钟' $title
$c['5,1']=V '产品名称与代号' $mainLabel; $c['5,2']=V 'WiTime ESP32-S3 Wi-Fi 点阵天气时钟 / WiTime-S3-WeatherClock / 固件 V0.1.0' $mainText
$c['6,1']=V '产品效果图' $mainLabel; $c['6,2']=V '显示效果：16×16 RGB 点阵显示时:分、秒、温度、天气图标和 Wi-Fi 状态；开机为环形时钟动画；联网成功显示绿色勾选，失败显示红色叉号；雷达检测到人时亮度 60%，无人时亮度 20%。' $mainText
$c['8,1']=V '产品概述' $mainLabel; $c['8,2']=V '基于 ESP32-S3、ESP-IDF 与 FreeRTOS 的室内 Wi-Fi 点阵天气时钟。DS3231 在断网后保持时间；联网后 SNTP 校时并回写 RTC；IP 定位后获取 Open-Meteo 实时温度/天气；Wi-Fi APSTA 和 BluFi 配网并行运行；LD2410C 用于人体存在检测和显示亮度联动。' $mainText
Add-Row $c 9 @('系统构成','ESP32-S3 主控','DS3231 RTC','WS2812 16×16','LD2410C 雷达','Wi-Fi APSTA','BluFi 配网','NVS 配置','FreeRTOS') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 10 @('主要参数','I2C0：GPIO8/9','LED：GPIO10','UART1：GPIO1/2','OUT：GPIO13','2.4GHz Wi-Fi','BLE / NimBLE','最多 5 组 Wi-Fi','多任务运行') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
$c['11,1']=V '产品选型' $mainLabel; $c['11,2']=V '核心模块、接口与用途' $mainGrid
Add-Row $c 12 @('','主控','显示','RTC','雷达','Wi-Fi','配网','存储','供电') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 13 @('',"ESP32-S3`nWi-Fi + BLE", "WS2812`n16×16 / 256px", "DS3231`nI2C0 / 0x68", "LD2410C`nOUT + UART1", "APSTA 2.4GHz`n自动选优", "BluFi + NimBLE`n安全协商", "NVS`n最多 5 组 Wi-Fi", "5V 点阵 / 3.3V 逻辑`nGND 共地") @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
$c['14,1']=V '单片机引脚图' $mainLabel; $c['14,2']=V 'ESP32-S3 引脚分配与外设连接' $mainGrid
Add-Row $c 15 @('','GPIO8','I2C0 SDA','DS3231 SDA','GPIO9','I2C0 SCL','DS3231 SCL','400kHz','3.3V') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 16 @('','GPIO10','RMT 输出','WS2812 DIN','16×16 / 256px','独立 5V','330Ω 串联','GND 共地','RGB 显示') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 17 @('','GPIO1','UART1 TX','LD2410C RX','GPIO2','UART1 RX','LD2410C TX','256000bps','GPIO13 OUT') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
$c['18,1']=V '功能清单' $mainLabel; $c['18,2']=V '已实现功能分项' $mainGrid
Add-Row $c 19 @('','点阵布局','时间显示','启动动画','RTC 读取','断网保持','网络校时','天气显示','显示可靠性') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 20 @('',"16×16 / 256 像素`n逐行蛇形映射，支持镜像/旋转", "上部 3×5 字模显示时:分`n右侧纵向显示秒", "环形时钟启动动画`n最短 8 秒，动画与网络任务并行", "I2C0 读取 DS3231 时间寄存器`n每秒更新本地时间快照", "断网/重启继续使用 RTC`n读取失败后周期重试，不阻塞显示", "获得 IP 后立即启动双 NTP 校时`n成功后更新系统时间并回写 DS3231", "显示温度与 5×5 天气图标`n晴/多云/雨/雪/雷雨状态映射", "点阵刷新失败延时 1ms 重试`n异常累计写入诊断日志") @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 21 @('','BluFi 配网','APSTA 模式','自动选网','断线重连','失败提示','人体感应','亮度联动','诊断监测') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 22 @('',"BLE 广播、扫描热点`n接收 SSID/密码并上报连接状态", "AP 常驻作为兜底热点`nSTA 用于连接路由器和互联网", "保存最多 5 组 Wi-Fi`n优先连接可用且信号更优的 SSID", "快速重试 1000ms`n后台重试 10000ms，连续失败后重新扫描", "连续 3 次连接失败`n显示红色失败页 1.5 秒后后台继续", "GPIO13 OUT + UART1 距离帧`n50ms 采样，阈值 ≤500mm", "有人设为 60%`n无人设为 20%，状态变化立即刷新", "启动、网络、校时、天气事件写入 NVS`n每秒检查堆与任务栈余量") @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
$c['23,1']=V '第三方接口调用' $mainLabel; $c['23,2']=V '外部网络服务调用' $mainGrid
Add-Row $c 24 @('','NTP 主服务','NTP 备服务','时间触发','校时结果','定位主服务','定位备服务','定位字段','位置变化') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 25 @('',"time.cloudflare.com`n主服务器", "time.google.com`n主服务 3000ms 超时后等待备服务", "设备获取 STA IP 后立即请求`n无需等待定位完成", "SNTP 成功后校正系统时间`n并写回 DS3231 供下次冷启动", "https://ipwho.is/`n优先读取位置服务", "https://ipapi.co/json/`nipwho.is 失败时自动回退", "纬度、经度、IANA 时区`n范围与非空值校验", "经纬度变化 ≥0.05° 或时区变化`n触发天气刷新") @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 26 @('','天气接口','请求参数','当前数据','刷新触发','定时刷新','无位置处理','请求异常','数据降级') @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
Add-Row $c 27 @('',"https://api.open-meteo.com/v1/forecast`nHTTPS GET", "latitude / longitude`ncurrent=temperature_2m,weather_code`ntimezone=auto", "解析 temperature_2m 与 weather_code`n转换为显示温度和天气图标", "联网成功、已有位置、位置更新时`n立即请求天气", "等待 600000ms（10 分钟）`n仅在 STA 已连接时执行", "无有效经纬度时跳过请求`n等待下一次定位成功", "HTTP 非 200、响应超 2048B、JSON 失败`n写入天气失败事件", "保留上一次有效天气快照`n无网络时可回退显示 DS3231 芯片温度") @($mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid,$mainGrid)
$sheets += @{name='A 产品开发总表'; xml=(Build-Sheet 'A 产品开发总表' $c @('A1:I3','B5:I5','B6:I6','B8:I8','B11:I11','B14:I14','B18:I18','B23:I23') @{1=18;2=14;3=14;4=14;5=14;6=14;7=14;8=14;9=14} @{1=29;2=29;3=29;5=22;6=46;7=14;8=52;9=28;10=28;11=22;12=30;13=52;14=22;15=28;16=30;17=30;18=22;19=30;20=100;21=30;22=100;23=22;24=30;25=100;26=30;27=112})}

# B 产品概述与定位
$c=@{}; $c['1,1']=V 'B 产品概述与定位' $title
Add-Row $c 3 @('项目维度','内容','填写/确认人','状态','资料来源') @($header,$header,$header,$header,$header)
$rows=@(
@('产品名称','WiTime ESP32-S3 Wi-Fi 时钟','待填写','待确认','README.md'),
@('产品定位','桌面/室内点阵时钟，提供网络时间、天气和人体感应亮度联动','待填写','待确认','README.md'),
@('目标用户','待填写','待填写','待确认','用户调研/需求评审记录'),
@('使用场景','家庭、办公桌面、展示空间等室内环境；具体场景待确认','待填写','待确认','待填写'),
@('核心卖点','离线 RTC 保持、网络校时、点阵信息展示、低亮待机/有人提亮','待填写','待确认','README.md；main/main.c'),
@('非目标范围','移动端 App、云端账号、远程控制等是否纳入本期：待填写','待填写','待确认','待填写'),
@('产品版本','硬件版本：待填写；固件版本：0.1.0','待填写','进行中','VERSION'),
@('计划周期','立项/EVT/DVT/PVT/量产日期：待填写','待填写','未开始','项目计划'),
@('交付物','样机、固件、源码、BOM、图纸、测试报告、使用说明','待填写','待确认','本模板 L 页')
)
$r=4; foreach($row in $rows){Add-Row $c $r $row @($label,$known,$input,$input,$note);$r++}
$sheets += @{name='B 产品概述与定位';xml=(Build-Sheet 'B 产品概述与定位' $c @('A1:E1') @{1=18;2=64;3=16;4=14;5=30} @{1=32;3=24;4=30;5=44;6=30;7=42;8=42;9=30;10=30;11=30;12=30} @(@{r='D4:D20';v='未开始,进行中,已完成,阻塞,待确认'}))}

# C 需求清单
$c=@{};$c['1,1']=V 'C 需求清单' $title
Add-Row $c 3 @('编号','类别','需求描述','优先级','验收标准','验收方式','负责人','状态','实测/结论','关联资料') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$reqs=@(
@('REQ-001','显示','16×16 点阵显示时:分、秒、天气图标和温度','P0','主页面元素显示完整，无错位、乱码或闪烁','样机目检/录像','待填写','进行中','待填写','main/APP/app_display.c'),
@('REQ-002','RTC','断网后可从 DS3231 读取有效时间并显示','P0','断网重启后时间可用，误差指标待填写','断网重启测试','待填写','进行中','待填写','main/APP/app_rtc.c'),
@('REQ-003','网络校时','联网后通过 SNTP 校时并回写 DS3231','P0','获得网络时间后系统时间与 RTC 同步成功','抓日志/断电复测','待填写','进行中','待填写','main/APP/app_time.c'),
@('REQ-004','配网','支持 BluFi 下发 STA Wi-Fi 信息','P0','可扫描 AP、下发 SSID/密码并获取连接状态','手机配网测试','待填写','进行中','待填写','main/middle/ble_provision.c'),
@('REQ-005','Wi-Fi','保存多个 Wi-Fi 配置并优先连接可用信号','P1','最多保存 5 组；选择规则符合设计','多 AP 场景测试','待填写','进行中','待填写','main/common/app_config.h'),
@('REQ-006','天气','位置有效时请求 Open-Meteo 并显示温度/天气码','P1','HTTP 200 且解析结果在点阵正确呈现','网络抓包/目检','待填写','进行中','待填写','main/APP/app_weather.c'),
@('REQ-007','感应','LD2410C 识别 0.5m 内有人状态','P1','距离 ≤500mm 时状态切换为有人','距离标尺测试','待填写','进行中','待填写','main/drivers/radar_presence.c'),
@('REQ-008','亮度','有人亮度 60%，无人亮度 20%','P1','状态变化后显示亮度切换，无持续抖动','目检/日志','待填写','进行中','待填写','main/main.c'),
@('REQ-009','启动','开机启动动画最少显示 8 秒，网络任务并行启动','P1','启动时序满足需求，进入主页面无长时间黑屏','视频计时/日志','待填写','待确认','README.md'),
@('REQ-010','容错','连续 3 次 Wi-Fi 失败显示失败提示，后台重试','P1','失败提示显示 1.5 秒，重试不阻塞主显示','断网/日志测试','待填写','待确认','README.md'),
@('REQ-011','可靠性','运行日志记录启动、网络、天气等关键事件','P2','关键异常可从日志定位','串口日志检查','待填写','进行中','待填写','main/common/runtime_event_log.c'),
@('REQ-012','安全','BluFi 配网采用安全协商/加密回调','P2','安全初始化、加密、校验流程可运行','代码审查/配网测试','待填写','待确认','main/middle/blufi_security.c')
)
$r=4;foreach($row in $reqs){Add-Row $c $r $row @($center,$label,$known,$input,$input,$input,$input,$input,$input,$note);$r++}
$sheets += @{name='C 需求清单';xml=(Build-Sheet 'C 需求清单' $c @('A1:J1') @{1=12;2=12;3=46;4=10;5=40;6=22;7=14;8=14;9=30;10=32} @{1=32;3=24;4=52;5=52;6=52;7=52;8=52;9=52;10=52;11=52;12=52;13=52;14=52;15=52} @(@{r='D4:D54';v='P0,P1,P2,P3'},@{r='H4:H54';v='未开始,进行中,已完成,阻塞,待确认'}))}

# D 选型与BOM
$c=@{};$c['1,1']=V 'D 方案选型与 BOM' $title
Add-Row $c 3 @('序号','模块','推荐型号/方案','关键规格/接口','选型理由','备选方案','供应商/链接','单价(元)','数量','状态','确认依据') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$bom=@(
@('1','主控','ESP32-S3 模组/开发板（具体料号待填写）','Wi-Fi + BLE；ESP-IDF','现有固件已基于 ESP32-S3','待填写','待填写','待填写','1','待确认','README.md'),
@('2','实时时钟','DS3231 模块/芯片（具体料号待填写）','I2C0；地址 0x68；3.3V','断网后保持时间，并提供芯片温度','待填写','待填写','待填写','1','待确认','main/drivers/ds3231.c'),
@('3','显示','16×16 WS2812 点阵','256 像素；DIN GPIO10；5V 供电','彩色点阵可显示时钟/天气图标','待填写','待填写','待填写','1','待确认','README.md'),
@('4','人体感应','HLK-LD2410C','OUT + UART1；256000bps','检测人体存在与距离，控制亮度','待填写','待填写','待填写','1','待确认','开发资料/HLK LD2410C生命存在感应模组说明书V1.09.pdf'),
@('5','电源','5V 输入 + 3.3V 降压方案待填写','需满足点阵峰值电流；与 ESP32 共地','点阵 5V 供电，主控/RTC 3.3V','待填写','待填写','待填写','待填写','待确认','README.md'),
@('6','数据保护','WS2812 DIN 串联约 330Ω 电阻','数据线串联；阻值待实物确认','降低数据线振铃风险','待填写','待填写','待填写','1','待确认','README.md'),
@('7','结构件','外壳、面板、支架待填写','尺寸/材料/工艺待填写','满足安装、散热、透光和美观','待填写','待填写','待填写','待填写','未开始','待填写')
)
$r=4;foreach($row in $bom){Add-Row $c $r $row @($center,$label,$known,$known,$known,$input,$input,$input,$input,$input,$note);$r++}
$sheets += @{name='D 方案选型与BOM';xml=(Build-Sheet 'D 方案选型与BOM' $c @('A1:K1') @{1=8;2=14;3=30;4=28;5=30;6=20;7=25;8=12;9=10;10=14;11=38} @{1=32;3=24;4=46;5=46;6=46;7=46;8=46;9=46;10=46} @(@{r='J4:J40';v='未开始,进行中,已完成,阻塞,待确认'}))}

# E 硬件接口与引脚
$c=@{};$c['1,1']=V 'E 硬件接口与引脚' $title
Add-Row $c 3 @('序号','模块','信号','ESP32-S3 引脚/接口','方向','电平/速率','连接说明','实物核对','状态','证据') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$pins=@(
@('1','DS3231','SDA','GPIO8 / I2C0','双向','3.3V / I2C 400kHz','RTC SDA；需与主控共地','待填写','待确认','main/drivers/ds3231.c'),
@('2','DS3231','SCL','GPIO9 / I2C0','输出/双向','3.3V / I2C 400kHz','RTC SCL；代码启用内部上拉','待填写','待确认','main/drivers/ds3231.c'),
@('3','WS2812','DIN','GPIO10','输出','5V 数据；电平匹配待确认','建议串联约 330Ω，点阵独立 5V 供电并共地','待填写','待确认','README.md'),
@('4','LD2410C','OUT','GPIO13','输入','有效高','人体存在数字输出','待填写','待确认','main/drivers/radar_presence.c'),
@('5','LD2410C','UART TX','GPIO1 / UART1 TX','输出','256000bps','连接到雷达 RX；交叉连接待实物确认','待填写','待确认','main/drivers/radar_presence.c'),
@('6','LD2410C','UART RX','GPIO2 / UART1 RX','输入','256000bps','连接到雷达 TX；用于获取距离帧','待填写','待确认','main/drivers/radar_presence.c'),
@('7','供电','3V3','待填写','输出','3.3V','ESP32-S3、DS3231 电源；实际电流与纹波待测','待填写','待确认','待填写'),
@('8','供电','5V','待填写','输入','5V','WS2812 点阵电源；峰值电流、线径与保护待确认','待填写','待确认','README.md'),
@('9','调试','下载/日志串口','待填写','双向','待填写','下载、烧录和日志接口定义','待填写','未开始','待填写')
)
$r=4;foreach($row in $pins){Add-Row $c $r $row @($center,$label,$label,$known,$input,$input,$known,$input,$input,$note);$r++}
$sheets += @{name='E 硬件接口与引脚';xml=(Build-Sheet 'E 硬件接口与引脚' $c @('A1:J1') @{1=8;2=14;3=14;4=24;5=12;6=18;7=42;8=20;9=14;10=34} @{1=32;3=24;4=44;5=44;6=44;7=44;8=44;9=44;10=44;11=44;12=44} @(@{r='I4:I40';v='未开始,进行中,已完成,阻塞,待确认'}))}

# F 软件架构与任务
$c=@{};$c['1,1']=V 'F 软件架构与任务' $title
Add-Row $c 3 @('层级','模块/任务','主要职责','输入/依赖','输出/联动','异常与降级','代码位置','状态') @($header,$header,$header,$header,$header,$header,$header,$header)
$mods=@(
@('应用入口','app_main','初始化 NVS、配置、快照、RTC、显示、天气、雷达、Wi-Fi、BluFi、健康监控','ESP-IDF；NVS；各模块 init','创建服务并注册 Wi-Fi/雷达回调','雷达/健康初始化失败后继续运行','main/main.c','进行中'),
@('应用服务','app_rtc','读取/写入 DS3231，提供 RTC 温度','DS3231 I2C 驱动','有效时间与温度快照','读取失败时由网络时间/已有快照兜底','main/APP/app_rtc.c','进行中'),
@('应用服务','app_time','SNTP 校时、定位、时区处理','Wi-Fi；HTTP；SNTP','系统时间、位置快照、RTC 回写','SNTP 超时 3000ms；保留 RTC 时间','main/APP/app_time.c','进行中'),
@('应用服务','app_weather','请求 Open-Meteo，解析当前温度/天气码','位置快照；Wi-Fi；HTTPS','天气快照、刷新显示','无有效位置时跳过请求；保留上次天气/RTC 温度','main/APP/app_weather.c','进行中'),
@('应用服务','app_display / display_task','唯一控制 WS2812，绘制启动、主页面、网络提示','时间/天气/Wi-Fi/雷达状态','点阵帧输出','刷新失败重试；保持帧更新','main/APP/app_display.c','进行中'),
@('驱动','wifi_manager','APSTA、STA 配置、扫描、自动重连、状态查询','NVS Wi-Fi 配置；ESP-WiFi','网络状态回调与连接信息','快速/后台重试；连续失败计数','main/drivers/wifi_manager.c','进行中'),
@('中间件','ble_provision','BluFi 广播、AP 扫描、配置下发和连接状态上报','NimBLE；Wi-Fi 管理器','写入 Wi-Fi/AP/BLE 配置','错误码上报；断开后恢复广播','main/middle/ble_provision.c','进行中'),
@('驱动','radar_presence','读取 LD2410C OUT/UART 数据帧，发布存在状态和距离','GPIO13；UART1 256000bps','感应回调，亮度联动','帧头/长度/尾部校验，无效帧计数','main/drivers/radar_presence.c','进行中'),
@('基础服务','runtime_event_log / app_health','运行事件记录与任务健康监控','各任务句柄与事件','诊断日志','初始化失败可继续运行','main/common/runtime_event_log.c；main/APP/app_health.c','进行中')
)
$r=4;foreach($row in $mods){Add-Row $c $r $row @($label,$label,$known,$known,$known,$known,$note,$input);$r++}
$sheets += @{name='F 软件架构与任务';xml=(Build-Sheet 'F 软件架构与任务' $c @('A1:H1') @{1=14;2=26;3=38;4=36;5=36;6=42;7=35;8=14} @{1=32;3=24;4=56;5=56;6=56;7=56;8=56;9=56;10=56;11=56;12=56} @(@{r='H4:H40';v='未开始,进行中,已完成,阻塞,待确认'}))}

# G 功能与交互流程
$c=@{};$c['1,1']=V 'G 功能清单与交互流程' $title
Add-Row $c 3 @('编号','功能/场景','触发条件','系统处理流程','用户可见结果','异常处理','验收要点','状态') @($header,$header,$header,$header,$header,$header,$header,$header)
$flows=@(
@('FLOW-01','首次上电/启动','设备上电','初始化 RTC、显示、网络、天气和感应；播放启动动画','启动动画后进入主页面','初始化失败写日志，按模块降级','启动到主页面时序、无黑屏','待确认'),
@('FLOW-02','已保存 Wi-Fi','存在可用 STA 配置','选择可用/较强信号的已保存 Wi-Fi 并连接','显示联网成功提示后继续主页面','连接失败累计计数并后台重试','多网络选择、网络切换','待确认'),
@('FLOW-03','无 Wi-Fi 配置','无保存网络','保持 APSTA 与 BluFi 广播可用','可由手机扫描/下发网络信息','配置字段校验、错误状态反馈','配网成功率、重启后记忆','待确认'),
@('FLOW-04','网络校时','获取 IP','启动 SNTP；成功后设置系统时间并写 DS3231','时间变为网络校准值','SNTP 超时后使用 RTC 时间','断网/恢复网络对比','待确认'),
@('FLOW-05','天气刷新','网络及有效位置','请求 Open-Meteo current 数据并解析温度、天气码','点阵显示天气图标和温度','无位置/HTTP/解析失败时保留上次结果','不同天气码显示','待确认'),
@('FLOW-06','人体感应','LD2410C 目标距离 ≤500mm','发布有人状态，回调设置亮度 60%','点阵变亮','无有效帧或距离超阈值进入无人状态','多人/静止人体/边界距离','待确认'),
@('FLOW-07','无人状态','未检测到目标或距离 >500mm','设置亮度 20%','点阵低亮显示','避免状态抖动和频繁刷新','亮度切换稳定性','待确认'),
@('FLOW-08','Wi-Fi 连续失败','失败次数达到 3','显示失败提示 1.5s 后继续后台连接','主页面保持可用，提示短暂出现','不阻塞 RTC/显示任务','无路由器情况下长时间运行','待确认')
)
$r=4;foreach($row in $flows){Add-Row $c $r $row @($center,$label,$known,$known,$known,$known,$input,$input);$r++}
$sheets += @{name='G 功能清单与交互流程';xml=(Build-Sheet 'G 功能清单与交互流程' $c @('A1:H1') @{1=12;2=22;3=24;4=50;5=34;6=36;7=28;8=14} @{1=32;3=24;4=56;5=56;6=56;7=56;8=56;9=56;10=56;11=56} @(@{r='H4:H40';v='未开始,进行中,已完成,阻塞,待确认'}))}

# H 关键参数
$c=@{};$c['1,1']=V 'H 关键参数与边界条件' $title
Add-Row $c 3 @('类别','参数项','当前代码值/已知事实','目标值/允差','实测值','单位','确认方式','状态','备注') @($header,$header,$header,$header,$header,$header,$header,$header,$header)
$params=@(
@('显示','点阵尺寸','16 × 16；256 像素','待填写','待填写','像素','目检/规格书','待确认','支持蛇形映射、镜像和旋转'),
@('显示','刷新周期','50','待填写','待填写','ms','日志/示波器','待确认','DISPLAY_REFRESH_PERIOD_MS'),
@('显示','有人亮度','60','待填写','待填写','%','目检/电流','待确认','雷达回调设置'),
@('显示','无人亮度','20','待填写','待填写','%','目检/电流','待确认','雷达回调设置'),
@('启动','启动动画最短时间','8','待填写','待填写','s','视频计时','待确认','README 业务描述'),
@('网络','Wi-Fi 快速重试间隔','1000','待填写','待填写','ms','串口日志','待确认','WIFI_FAST_RETRY_DELAY_MS'),
@('网络','Wi-Fi 后台重试间隔','10000','待填写','待填写','ms','串口日志','待确认','WIFI_BACKGROUND_RETRY_DELAY_MS'),
@('网络','SNTP 服务超时','3000','待填写','待填写','ms','日志/抓包','待确认','SNTP_SERVER_TIMEOUT_MS'),
@('天气','天气刷新周期','10','待填写','待填写','min','日志/抓包','待确认','README 业务描述'),
@('雷达','UART 波特率','256000','待填写','待填写','bps','串口测试','待确认','RADAR_UART_BAUD_RATE'),
@('雷达','采样周期','50','待填写','待填写','ms','日志/示波器','待确认','RADAR_SAMPLE_PERIOD_MS'),
@('雷达','有人距离阈值','500','待填写','待填写','mm','距离标尺','待确认','RADAR_DISTANCE_THRESHOLD_MM'),
@('电源','WS2812 供电','独立 5V，需与 ESP32 共地','待填写','待填写','V','万用表/示波器','待确认','README.md'),
@('环境','工作温湿度','待填写','待填写','待填写','°C / %RH','环境试验','未开始','需结合器件规格确认')
)
$r=4;foreach($row in $params){Add-Row $c $r $row @($label,$label,$known,$input,$input,$input,$input,$input,$note);$r++}
$sheets += @{name='H 关键参数与边界';xml=(Build-Sheet 'H 关键参数与边界' $c @('A1:I1') @{1=14;2=22;3=34;4=18;5=18;6=12;7=20;8=14;9=30} @{1=32;3=24;4=36;5=36;6=36;7=36;8=36;9=36;10=36;11=36;12=36;13=36;14=36;15=36;16=36;17=36} @(@{r='H4:H50';v='未开始,进行中,已完成,阻塞,待确认'}))}

# I 测试
$c=@{};$c['1,1']=V 'I 测试计划与记录' $title
Add-Row $c 3 @('测试编号','类别','测试项','前置条件','步骤','预期结果','实测结果','结论','测试人','日期','问题单号') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$tests=@(
@('T-001','基础功能','开机显示','已烧录目标固件','断电后上电并录像','启动动画后进入主页面','待填写','待测试','待填写','待填写','待填写'),
@('T-002','RTC','断网保持时间','DS3231 已校准','断网、断电、等待后重新上电','可读取并显示有效 RTC 时间','待填写','待测试','待填写','待填写','待填写'),
@('T-003','网络','BluFi 配网','手机已安装测试工具，设备无 Wi-Fi 配置','扫描、选择 AP、输入密码、下发','设备连接路由器并报告状态','待填写','待测试','待填写','待填写','待填写'),
@('T-004','网络','三次失败重试','配置不可用 SSID 或关闭路由器','观察失败次数、提示和后台重试','第三次失败显示 1.5s 提示，主页面不阻塞','待填写','待测试','待填写','待填写','待填写'),
@('T-005','校时','SNTP 校时与 RTC 回写','设备已联网，RTC 时间故意设偏','连接网络并重启/断网复测','系统时间校准，RTC 保持新时间','待填写','待测试','待填写','待填写','待填写'),
@('T-006','天气','天气更新','网络与有效定位可用','等待刷新或触发网络通知','温度和天气码正确显示','待填写','待测试','待填写','待填写','待填写'),
@('T-007','感应','近距离有人','LD2410C 连通，准备距离标尺','在 ≤500mm 与 >500mm 两位置测试','有人 60%，无人 20% 亮度','待填写','待测试','待填写','待填写','待填写'),
@('T-008','稳定性','连续运行','稳定供电和网络环境','连续运行 24h/72h（填写实际时长）','无死机、花屏、异常重启或资源异常','待填写','待测试','待填写','待填写','待填写'),
@('T-009','电源','点阵峰值电流与压降','具备电流表/示波器','全亮/启动/动画/主页面分别测试','供电不低于目标，器件无异常发热','待填写','待测试','待填写','待填写','待填写'),
@('T-010','回归','版本回归','发布候选固件','执行 T-001 至 T-009','所有 P0/P1 项通过或有书面豁免','待填写','待测试','待填写','待填写','待填写')
)
$r=4;foreach($row in $tests){Add-Row $c $r $row @($center,$label,$label,$input,$input,$input,$input,$input,$input,$input,$input);$r++}
$sheets += @{name='I 测试计划与记录';xml=(Build-Sheet 'I 测试计划与记录' $c @('A1:K1') @{1=12;2=14;3=22;4=26;5=40;6=42;7=36;8=14;9=14;10=14;11=16} @{1=32;3=24;4=48;5=56;6=56;7=56;8=56;9=56;10=56;11=56;12=56;13=56;14=56} @(@{r='H4:H100';v='待测试,通过,不通过,阻塞,豁免'}))}

# J 风险问题
$c=@{};$c['1,1']=V 'J 风险问题与闭环' $title
Add-Row $c 3 @('编号','类型','风险/问题描述','影响','等级','临时措施','根因分析','纠正/预防措施','负责人','截止日期','状态','关闭依据') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$issues=@(
@('R-001','硬件','WS2812 5V 供电与 ESP32-S3 3.3V 数据电平兼容性待实物验证','可能花屏/无法点亮','高','先在样机进行全亮、动画和长线测试','待填写','必要时增加电平转换、优化接地/去耦','待填写','待填写','待确认','待填写'),
@('R-002','电源','点阵高亮时峰值电流与压降未实测','可能重启、闪烁或发热','高','限制默认亮度并进行电流测试','待填写','确认电源额定、电线线径、保险与散热','待填写','待填写','待确认','待填写'),
@('R-003','网络','天气/定位依赖公共网络服务可用性','天气无法更新','中','保留上次结果与 RTC 温度','待填写','建立超时、错误日志和替代服务评估','待填写','待填写','待确认','待填写'),
@('R-004','雷达','LD2410C 灵敏度、安装位置与误触发未验证','亮度频繁切换/体验不佳','中','采用 500mm 阈值进行初测','待填写','按实际环境调整门限、安装位置和滤波策略','待填写','待填写','待确认','待填写'),
@('R-005','结构','外壳材料、透光、散热与安装方式未定义','外观/可靠性不确定','中','完成结构需求收集','待填写','输出 3D 图、打样、装配与温升验证','待填写','待填写','未开始','待填写')
)
$r=4;foreach($row in $issues){Add-Row $c $r $row @($center,$label,$known,$known,$input,$input,$input,$input,$input,$input,$input,$input);$r++}
$sheets += @{name='J 风险问题与闭环';xml=(Build-Sheet 'J 风险问题与闭环' $c @('A1:L1') @{1=10;2=12;3=36;4=26;5=10;6=32;7=32;8=38;9=14;10=14;11=14;12=30} @{1=32;3=24;4=58;5=58;6=58;7=58;8=58;9=58} @(@{r='E4:E100';v='高,中,低'},@{r='K4:K100';v='未开始,进行中,已关闭,阻塞,待确认'}))}

# K 版本
$c=@{};$c['1,1']=V 'K 版本与变更记录' $title
Add-Row $c 3 @('序号','日期','类别','版本/文件','变更内容','变更原因','影响范围','提出人','确认人','状态') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$changes=@(
@('1','待填写','固件','0.1.0','建立 ESP32-S3 Wi-Fi 时钟基础工程','项目初始化','主控、RTC、显示、网络','待填写','待填写','已记录'),
@('2','待填写','硬件','待填写','填写原理图/PCB/样机版本变更','待填写','待填写','待填写','待填写','待填写'),
@('3','待填写','结构','待填写','填写外壳/面板/安装方式变更','待填写','待填写','待填写','待填写','待填写'),
@('4','待填写','文档','本模板','建立项目开发资料填写模板','项目管理与验收需要','全部交付文档','待填写','待填写','已记录')
)
$r=4;foreach($row in $changes){Add-Row $c $r $row @($center,$input,$input,$input,$input,$input,$input,$input,$input,$input);$r++}
$sheets += @{name='K 版本与变更记录';xml=(Build-Sheet 'K 版本与变更记录' $c @('A1:J1') @{1=8;2=14;3=14;4=22;5=44;6=32;7=28;8=14;9=14;10=14} @{1=32;3=24;4=46;5=46;6=46;7=46;8=46} @(@{r='J4:J100';v='待填写,已记录,已评审,已发布'}))}

# L 验收与交付
$c=@{};$c['1,1']=V 'L 验收与交付清单' $title
Add-Row $c 3 @('编号','交付类别','交付项','版本/编号','存放位置/链接','提交人','审核人','提交日期','验收标准','状态','备注') @($header,$header,$header,$header,$header,$header,$header,$header,$header,$header,$header)
$delivery=@(
@('1','样机','整机样机','待填写','待填写','待填写','待填写','待填写','外观、功能、供电和安全检查通过','未开始','附样机照片/编号'),
@('2','硬件','原理图','待填写','待填写','待填写','待填写','待填写','接口、电源、关键器件标注完整','未开始',''),
@('3','硬件','PCB Gerber/坐标/BOM','待填写','待填写','待填写','待填写','待填写','生产资料齐全、BOM 可采购','未开始',''),
@('4','固件','发布固件','待填写','release/ 或待填写','待填写','待填写','待填写','版本可追溯，烧录和回归通过','未开始',''),
@('5','软件','源码与构建说明','待填写','D:\\Project\\WiTime','待填写','待填写','待填写','可构建、README 完整、依赖明确','进行中',''),
@('6','测试','测试报告','待填写','待填写','待填写','待填写','待填写','P0/P1 测试通过或有批准豁免','未开始',''),
@('7','文档','产品说明/使用指南','待填写','待填写','待填写','待填写','待填写','配网、使用、异常说明完整','未开始',''),
@('8','管理','项目开发资料填写模板','本文件','outputs/witime_project_template_20260730','待填写','待填写','待填写','字段完整、关键内容可追溯','进行中','')
)
$r=4;foreach($row in $delivery){Add-Row $c $r $row @($center,$label,$label,$input,$input,$input,$input,$input,$input,$input,$input);$r++}
$sheets += @{name='L 验收与交付清单';xml=(Build-Sheet 'L 验收与交付清单' $c @('A1:K1') @{1=8;2=14;3=24;4=18;5=34;6=14;7=14;8=14;9=38;10=14;11=22} @{1=32;3=24;4=44;5=44;6=44;7=44;8=44;9=44;10=44;11=44} @(@{r='J4:J100';v='未开始,进行中,已提交,已验收,阻塞'}))}

if ($SingleSheet) { $sheets = @($sheets | Where-Object { $_.name -eq 'A 产品开发总表' }) }
$sheetList = ($sheets | ForEach-Object -Begin {$i=1} -Process { '<sheet name="' + (Escape-Xml $_.name) + '" sheetId="' + $i + '" r:id="rId' + $i + '"/>'; $i++ }) -join ''
$rels = ($sheets | ForEach-Object -Begin {$i=1} -Process { '<Relationship Id="rId' + $i + '" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet' + $i + '.xml"/>'; $i++ }) -join ''
$contentTypes = ($sheets | ForEach-Object -Begin {$i=1} -Process { '<Override PartName="/xl/worksheets/sheet' + $i + '.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'; $i++ }) -join ''

$styles = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="1"><numFmt numFmtId="164" formatCode="0%"/></numFmts>
<fonts count="5"><font><sz val="10"/><name val="Microsoft YaHei"/></font><font><b/><sz val="22"/><name val="Microsoft YaHei"/></font><font><b/><sz val="12"/><name val="Microsoft YaHei"/></font><font><b/><color rgb="FFFFFFFF"/><sz val="10"/><name val="Microsoft YaHei"/></font><font><color rgb="FF666666"/><i/><sz val="10"/><name val="Microsoft YaHei"/></font></fonts>
<fills count="7"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FFFFFFFF"/><bgColor indexed="64"/></patternFill></fill><fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/><bgColor indexed="64"/></patternFill></fill><fill><patternFill patternType="solid"><fgColor rgb="FFD9EAF7"/><bgColor indexed="64"/></patternFill></fill><fill><patternFill patternType="solid"><fgColor rgb="FFE2F0D9"/><bgColor indexed="64"/></patternFill></fill><fill><patternFill patternType="solid"><fgColor rgb="FFFFF2CC"/><bgColor indexed="64"/></patternFill></fill></fills>
<borders count="3"><border><left/><right/><top/><bottom/><diagonal/></border><border><left style="thin"><color rgb="FFB7C9D6"/></left><right style="thin"><color rgb="FFB7C9D6"/></right><top style="thin"><color rgb="FFB7C9D6"/></top><bottom style="thin"><color rgb="FFB7C9D6"/></bottom><diagonal/></border><border><left style="thin"><color rgb="FF808080"/></left><right style="thin"><color rgb="FF808080"/></right><top style="thin"><color rgb="FF808080"/></top><bottom style="thin"><color rgb="FF808080"/></bottom><diagonal/></border></borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="15">
<xf numFmtId="0" fontId="0" fillId="2" borderId="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="1" fillId="2" borderId="0" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
<xf numFmtId="0" fontId="2" fillId="4" borderId="1" applyAlignment="1"><alignment horizontal="left" vertical="center"/></xf>
<xf numFmtId="0" fontId="3" fillId="3" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="2" fillId="4" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="5" borderId="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="6" borderId="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="164" fontId="2" fillId="5" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
<xf numFmtId="0" fontId="4" fillId="2" borderId="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="2" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="2" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="2" borderId="2" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles></styleSheet>
'@

$workbook = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><workbookPr/><bookViews><workbookView activeTab="0"/></bookViews><sheets>' + $sheetList + '</sheets></workbook>'
$workbookRels = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">' + $rels + '<Relationship Id="rIdStyles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/></Relationships>'
$rootRels = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/><Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/></Relationships>'
$types = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>' + $contentTypes + '<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/><Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/></Types>'
$core = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"><dc:title>WiTime 项目开发资料填写模板</dc:title><dc:creator>Codex</dc:creator><dcterms:created xsi:type="dcterms:W3CDTF">2026-07-30T00:00:00Z</dcterms:created></cp:coreProperties>'
$app = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties" xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes"><Application>Microsoft Excel</Application></Properties>'

$resolved = Join-Path (Get-Location) $OutputPath
$outDir = Split-Path -Parent $resolved
$temp = Join-Path $env:TEMP ('witime_xlsx_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    New-Item -ItemType Directory -Path (Join-Path $temp '_rels'),(Join-Path $temp 'xl/_rels'),(Join-Path $temp 'xl/worksheets'),(Join-Path $temp 'docProps') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $temp '[Content_Types].xml'),$types,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp '_rels/.rels'),$rootRels,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp 'xl/workbook.xml'),$workbook,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp 'xl/_rels/workbook.xml.rels'),$workbookRels,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp 'xl/styles.xml'),$styles,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp 'docProps/core.xml'),$core,[System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $temp 'docProps/app.xml'),$app,[System.Text.UTF8Encoding]::new($false))
    for ($i=0; $i -lt $sheets.Count; $i++) { [System.IO.File]::WriteAllText((Join-Path $temp ("xl/worksheets/sheet" + ($i+1) + '.xml')),$sheets[$i].xml,[System.Text.UTF8Encoding]::new($false)) }
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Force }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($temp,$resolved,[System.IO.Compression.CompressionLevel]::Optimal,$false)
    Write-Output $resolved
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
