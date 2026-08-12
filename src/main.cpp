#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <img_converters.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <netinet/tcp.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace {

constexpr char kAccessPointName[] = "ESP32-CAM-DAY1";
constexpr char kAccessPointPassword[] = "esp32cam";
constexpr char kTimelapseDir[] = "/timelapse";
constexpr char kDataDir[] = "/data";
constexpr char kFlowCsvPath[] = "/data/flow.csv";
constexpr char kCaptureStatePath[] = "/data/capture-state.txt";
constexpr char kCaptureStateTempPath[] = "/data/.capture-state.tmp";
constexpr char kPhotoCatalogPath[] = "/data/photo-index.bin";
constexpr char kStorageResetMarkerPath[] = "/.photos-reset-20260806-v1";
constexpr bool kRunPhotoResetOnBoot = false;
constexpr uint32_t kCaptureIntervalMs = 5000;
constexpr uint32_t kCapturesPerHour = 60UL * 60UL * 1000UL / kCaptureIntervalMs;
constexpr uint8_t kStreamTargetFps = 15;
constexpr uint32_t kStreamFrameIntervalMs =
    (1000UL + kStreamTargetFps - 1) / kStreamTargetFps;
constexpr uint64_t kStorageReserveBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint16_t kCaptureStateCheckpointImages = 20;
constexpr uint32_t kFlowSdLockTimeoutMs = 10000;
constexpr uint8_t kSoftwareJpegQuality = 80;
constexpr uint32_t kMotionSampleIntervalMs = 500;
constexpr uint32_t kApproachSampleIntervalMs = 200;
constexpr uint32_t kCameraMotionSampleIntervalMs = 1000;
constexpr uint32_t kCameraArmedMotionSampleIntervalMs = 500;
constexpr uint32_t kCameraApproachSampleIntervalMs = 200;
constexpr uint32_t kApproachTrackingTimeoutMs = 20000;
constexpr uint32_t kMotionCooldownMs = 5000;
constexpr uint8_t kMotionWarmupSamples = 6;
constexpr uint8_t kStableFramesToRearm = 4;
constexpr uint8_t kStableMotionFramesToRebase = 4;
constexpr uint8_t kApproachMissingFramesToConfirmExit = 2;
constexpr uint8_t kApproachMinActiveFrames = 3;
constexpr uint8_t kApproachPartialExitFrames = 2;
constexpr uint8_t kApproachRemainingPercent = 92;
constexpr uint16_t kApproachPartialExitDrop = 800;  // 8 percentage points.
constexpr uint16_t kApproachRemainingTargetMin = 1500;  // 15 percent.
constexpr uint32_t kApproachExitCooldownMs = 500;
constexpr uint16_t kMotionWidth = 80;
constexpr uint16_t kMotionHeight = 60;
constexpr uint16_t kRoiLeft = 0;
constexpr uint16_t kRoiTop = 7;
constexpr uint16_t kRoiWidth = 80;
constexpr uint16_t kRoiHeight = 46;
constexpr uint8_t kPixelDeltaThreshold = 15;
constexpr uint16_t kMotionRatioThreshold = 100;  // 1.00 percent.
constexpr uint16_t kSceneStableRatioThreshold = 100;  // 1.00 percent.
constexpr uint16_t kGlobalChangeThreshold = 10000;  // Keep full-frame human motion.
constexpr size_t kRoiPixels = kRoiWidth * kRoiHeight;
constexpr uint16_t kInvalidMotionCenterX = UINT16_MAX;
constexpr uint16_t kApproachFarScoreMax = 3500;  // 35 percent.
constexpr uint16_t kApproachNearScoreMin = 4500;  // 45 percent.
constexpr uint16_t kApproachRequiredGrowth = 1000;  // 10 percentage points.
constexpr uint32_t kPhotoRecoveryOldestHint = 2251;
constexpr uint8_t kExposureBrightPixelThreshold = 240;
constexpr uint8_t kExposureDarkPixelThreshold = 10;
constexpr uint8_t kExposureOverexposedMeanThreshold = 200;
constexpr uint8_t kExposureUnderexposedMeanThreshold = 10;
constexpr uint16_t kExposureOverexposedRatioThreshold = 3500;  // 35 percent.
constexpr uint16_t kExposureUnderexposedRatioThreshold = 8000;  // 80 percent.
constexpr uint8_t kExposureAbnormalFramesToRecover = 8;
constexpr uint8_t kExposureAutoSettleFrames = 16;
constexpr uint32_t kExposureRecoveryCooldownMs = 2UL * 60UL * 1000UL;

// AI Thinker ESP32-CAM camera pins.
constexpr int kPinPwdn = 32;
constexpr int kPinReset = -1;
constexpr int kPinXclk = 0;
constexpr int kPinSiod = 26;
constexpr int kPinSioc = 27;
constexpr int kPinY9 = 35;
constexpr int kPinY8 = 34;
constexpr int kPinY7 = 39;
constexpr int kPinY6 = 36;
constexpr int kPinY5 = 21;
constexpr int kPinY4 = 19;
constexpr int kPinY3 = 18;
constexpr int kPinY2 = 5;
constexpr int kPinVsync = 25;
constexpr int kPinHref = 23;
constexpr int kPinPclk = 22;

httpd_handle_t webServer = nullptr;
httpd_handle_t streamServer = nullptr;
DNSServer captiveDns;
bool captiveDnsStarted = false;
Preferences wifiPreferences;
SemaphoreHandle_t cameraMutex = nullptr;
SemaphoreHandle_t sdMutex = nullptr;
QueueHandle_t motionFrameQueue = nullptr;

volatile bool sdReady = false;
volatile uint32_t nextImageIndex = 1;
volatile uint32_t oldestImageIndex = 1;
volatile uint32_t savedThisBoot = 0;
volatile uint32_t captureFailures = 0;
volatile uint32_t lastImageBytes = 0;
volatile uint32_t lastWriteMs = 0;
volatile uint32_t freeSpaceMb = 0;
volatile uint32_t customerCount = 0;
volatile uint32_t customerCountDate = 0;
volatile uint16_t motionScore = 0;
volatile bool motionDetected = false;

uint8_t motionRgb565[kMotionWidth * kMotionHeight * 2];
uint8_t backgroundRoi[kRoiPixels];
uint8_t currentRoi[kRoiPixels];
uint8_t previousRoi[kRoiPixels];
bool motionBaselineReady = false;
uint16_t captureStateChanges = 0;
uint64_t estimatedFreeBytes = 0;
framesize_t activeFrameSize = FRAMESIZE_VGA;
volatile bool streamClientActive = false;
volatile bool motionTaskReady = false;
volatile bool approachTrackingActive = false;
volatile bool motionDetectorArmed = false;
volatile bool approachNearConfirmed = false;
volatile bool wifiRestartRequested = false;
volatile bool exposureAdjusting = false;
volatile uint8_t cameraBrightness = 0;
volatile uint16_t cameraBrightRatio = 0;
volatile uint16_t cameraDarkRatio = 0;
uint32_t wifiRestartRequestedAt = 0;

struct MotionFrame {
  uint8_t *jpeg;
  size_t length;
};

struct __attribute__((packed)) PhotoCatalogRecord {
  uint32_t index;
  int64_t epoch;
};

static_assert(sizeof(PhotoCatalogRecord) == 12, "photo catalog record size changed");

extern const uint8_t chartJsStart[] asm("_binary_web_chart_umd_min_js_start");
extern const uint8_t chartJsEnd[] asm("_binary_web_chart_umd_min_js_end");

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="theme-color" content="#ffffff">
  <title>客流监测网关</title>
  <style>
    :root{color-scheme:light;--ink:#17201b;--muted:#68736d;--line:#dfe5e1;--panel:#fff;--page:#f4f6f5;--green:#157347;--green-soft:#e7f4ec;--amber:#a15c00;--amber-soft:#fff2dc;--red:#b42318}
    *{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--page);color:var(--ink);font-family:Arial,"Microsoft YaHei",sans-serif;letter-spacing:0}
    button,select,input{font:inherit}button{cursor:pointer}header{height:58px;padding:0 max(18px,env(safe-area-inset-left));display:flex;align-items:center;justify-content:space-between;background:#fff;border-bottom:1px solid var(--line)}
    .brand{display:flex;align-items:center;gap:10px;min-width:0}.brand-mark{width:4px;height:27px;background:var(--green);border-radius:2px}.brand-copy{min-width:0}h1{font-size:17px;line-height:21px;margin:0;font-weight:750}.subtitle{font-size:11px;color:var(--muted);margin-top:1px}
    .connection{display:flex;align-items:center;gap:7px;font-size:12px;font-weight:700;color:var(--green);white-space:nowrap}.connection::before{content:"";width:8px;height:8px;border-radius:50%;background:currentColor}.connection.offline{color:var(--red)}
    .tabs-wrap{background:#fff;border-bottom:1px solid var(--line)}.tabs{max-width:920px;height:48px;margin:0 auto;padding:6px 18px;display:grid;grid-template-columns:repeat(3,1fr);gap:4px}.tab{border:0;border-radius:6px;background:transparent;color:var(--muted);font-size:13px;font-weight:700}.tab.active{background:var(--green-soft);color:var(--green)}
    main{max-width:920px;margin:0 auto;padding:18px}.view{display:none}.view.active{display:block}.camera-shell,.player-shell{position:relative;width:100%;aspect-ratio:4/3;background:#090b0a;border-radius:6px;overflow:hidden;box-shadow:0 1px 2px rgba(20,32,25,.12)}
    #stream{display:block;width:100%;height:100%;object-fit:contain}.camera-placeholder{position:absolute;inset:0;display:grid;place-items:center;color:#cbd2ce;font-size:13px;background:#111;pointer-events:none}.camera-placeholder.hidden{display:none}
    .camera-bar{position:absolute;left:0;right:0;top:0;min-height:44px;padding:8px 10px;display:flex;align-items:center;justify-content:space-between;background:rgba(0,0,0,.66);color:#fff}.camera-label{display:flex;align-items:center;gap:8px;font-size:12px;font-weight:700}.live-dot{width:7px;height:7px;border-radius:50%;background:#38d477}.camera-actions{display:flex;gap:7px}.icon-button{width:34px;height:34px;padding:0;border:1px solid rgba(255,255,255,.35);border-radius:6px;background:rgba(20,20,20,.7);color:#fff;display:grid;place-items:center;font-size:17px;cursor:pointer}.icon-button:active{background:#333}
    .summary{margin-top:14px;background:var(--panel);border:1px solid var(--line);border-radius:6px;display:grid;grid-template-columns:minmax(170px,1fr) minmax(220px,1.35fr)}.count-block{padding:18px 20px;border-right:1px solid var(--line)}.eyebrow{font-size:12px;color:var(--muted);font-weight:700}.count-row{display:flex;align-items:baseline;gap:8px;margin-top:4px}.count-value{font-size:44px;line-height:50px;font-weight:800;font-variant-numeric:tabular-nums}.unit{font-size:13px;color:var(--muted)}
    .motion-block{padding:18px 20px;display:flex;flex-direction:column;justify-content:center}.motion-head{display:flex;align-items:center;justify-content:space-between;gap:12px}.motion-state{font-size:14px;font-weight:750}.motion-state.active{color:var(--amber)}.motion-score{font-size:13px;color:var(--muted);font-variant-numeric:tabular-nums}.meter{height:7px;margin-top:12px;background:#edf0ee;border-radius:4px;overflow:hidden}.meter-fill{height:100%;width:0;background:var(--green);border-radius:4px;transition:width .25s ease,background-color .25s ease}.meter-fill.active{background:#e08a13}
    .metrics{margin-top:14px;display:grid;grid-template-columns:repeat(3,1fr);background:var(--panel);border:1px solid var(--line);border-radius:6px;overflow:hidden}.metric{min-width:0;padding:15px 16px;border-right:1px solid var(--line);border-bottom:1px solid var(--line)}.metric:nth-child(3n){border-right:0}.metric:nth-last-child(-n+3){border-bottom:0}.metric-label{display:block;font-size:11px;color:var(--muted);margin-bottom:6px}.metric-value{display:block;font-size:15px;font-weight:750;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-variant-numeric:tabular-nums}.metric-value.good{color:var(--green)}.metric-value.bad{color:var(--red)}
    .section-head{min-height:48px;margin-bottom:12px;display:flex;align-items:center;justify-content:space-between;gap:12px}.section-title{margin:0;font-size:18px}.section-meta{font-size:12px;color:var(--muted)}.tool-button{width:36px;height:36px;padding:0;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);font-size:19px}.chart-shell{position:relative;height:330px;padding:16px;background:#fff;border:1px solid var(--line);border-radius:6px}.chart-empty{position:absolute;inset:0;display:grid;place-items:center;color:var(--muted);font-size:13px;pointer-events:none}.chart-empty.hidden{display:none}.trend-kpis{margin-top:12px;display:grid;grid-template-columns:repeat(3,1fr);background:#fff;border:1px solid var(--line);border-radius:6px;overflow:hidden}.trend-kpi{padding:15px 16px;border-right:1px solid var(--line)}.trend-kpi:last-child{border-right:0}.trend-kpi strong{display:block;margin-top:5px;font-size:20px;font-variant-numeric:tabular-nums}.trend-kpi span{font-size:11px;color:var(--muted)}
    #playbackFrame{display:block;width:100%;height:100%;object-fit:contain}.player-placeholder{position:absolute;inset:0;display:grid;place-items:center;color:#d6dcda;font-size:13px;background:#111}.player-placeholder.hidden{display:none}.player-badge{position:absolute;top:10px;left:10px;padding:6px 8px;border-radius:5px;background:rgba(0,0,0,.68);color:#fff;font-size:11px;font-weight:700}.player-frame{position:absolute;right:10px;top:10px;padding:6px 8px;border-radius:5px;background:rgba(0,0,0,.68);color:#fff;font-size:11px;font-variant-numeric:tabular-nums}.player-controls{margin-top:12px;padding:12px;background:#fff;border:1px solid var(--line);border-radius:6px}.transport{display:grid;grid-template-columns:38px 38px 1fr 38px;align-items:center;gap:8px}.control-button{width:38px;height:38px;padding:0;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);font-size:17px}.control-button.primary{background:var(--green);border-color:var(--green);color:#fff}.timeline{width:100%;accent-color:var(--green)}.player-options{margin-top:10px;display:flex;align-items:center;justify-content:space-between;gap:12px}.player-settings{display:flex;align-items:center;gap:8px}.speed-control{height:34px;display:grid;grid-template-columns:repeat(3,38px);border:1px solid var(--line);border-radius:6px;overflow:hidden;background:#fff}.speed-button{padding:0;border:0;border-right:1px solid var(--line);background:#fff;color:var(--muted);font-size:12px;font-weight:700}.speed-button:last-child{border-right:0}.speed-button.active{background:var(--green-soft);color:var(--green)}.range-select{height:34px;min-width:126px;padding:0 30px 0 10px;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);font-size:12px}.playback-state{min-width:0;font-size:12px;color:var(--muted);font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
    footer{min-height:44px;padding:12px 2px calc(12px + env(safe-area-inset-bottom));display:flex;justify-content:space-between;gap:12px;color:var(--muted);font-size:11px}
    @media(max-width:640px){header{height:56px}.tabs{padding-left:10px;padding-right:10px}main{padding:0}.camera-shell,.player-shell{border-radius:0;box-shadow:none}.summary{margin:0;border-left:0;border-right:0;border-radius:0}.metrics{margin:10px;border-radius:6px;grid-template-columns:repeat(2,1fr)}.metric:nth-child(n){border-right:1px solid var(--line);border-bottom:1px solid var(--line)}.metric:nth-child(2n){border-right:0}.metric:nth-last-child(-n+2){border-bottom:0}.section-head{padding:8px 12px;margin:0}.chart-shell{height:285px;margin:0 10px;padding:10px}.trend-kpis{margin:10px;grid-template-columns:1fr}.trend-kpi{border-right:0;border-bottom:1px solid var(--line);display:flex;align-items:center;justify-content:space-between}.trend-kpi:last-child{border-bottom:0}.trend-kpi strong{margin:0;font-size:17px}.player-controls{margin:10px}.player-options{align-items:flex-end}footer{padding-left:12px;padding-right:12px}.count-value{font-size:40px;line-height:46px}}
    @media(max-width:430px){.summary{grid-template-columns:1fr}.count-block{border-right:0;border-bottom:1px solid var(--line);display:flex;align-items:center;justify-content:space-between;gap:16px}.count-row{margin:0}.motion-block{padding-top:15px;padding-bottom:15px}.subtitle{display:none}.player-options{align-items:stretch;flex-direction:column}.player-settings{justify-content:space-between}.range-select{flex:1;min-width:0}}
  </style>
</head>
<body>
  <header>
    <div class="brand"><span class="brand-mark"></span><div class="brand-copy"><h1>客流监测网关</h1><div class="subtitle">预览与照片 · 640 × 480</div></div></div>
    <div class="connection offline" id="connection">连接中</div>
  </header>
  <div class="tabs-wrap"><nav class="tabs" role="tablist" aria-label="数据视图">
    <button class="tab active" type="button" data-view="liveView" role="tab" aria-selected="true">实时</button>
    <button class="tab" type="button" data-view="trendView" role="tab" aria-selected="false">趋势</button>
    <button class="tab" type="button" data-view="timelapseView" role="tab" aria-selected="false">延时</button>
  </nav></div>
  <main>
    <section class="view active" id="liveView" role="tabpanel">
      <section class="camera-shell" id="cameraShell">
        <img id="stream" alt="实时摄像头画面">
        <div class="camera-placeholder" id="placeholder">正在载入实时画面…</div>
        <div class="camera-bar">
          <div class="camera-label"><span class="live-dot"></span><span>实时预览</span></div>
          <div class="camera-actions">
            <button class="icon-button" id="pauseButton" type="button" title="暂停预览" aria-label="暂停预览">Ⅱ</button>
            <button class="icon-button" id="fullscreenButton" type="button" title="全屏预览" aria-label="全屏预览">⛶</button>
          </div>
        </div>
      </section>
      <section class="summary">
        <div class="count-block"><div><div class="eyebrow">今日累计客流</div><div class="count-row"><span class="count-value" id="count">--</span><span class="unit">人次</span></div></div></div>
        <div class="motion-block">
          <div class="motion-head"><span class="motion-state" id="motionState">等待检测</span><span class="motion-score" id="motionScore">--%</span></div>
          <div class="meter" role="progressbar" aria-label="运动强度" aria-valuemin="0" aria-valuemax="100"><div class="meter-fill" id="motionBar"></div></div>
        </div>
      </section>
      <section class="metrics">
        <div class="metric"><span class="metric-label">SD 卡</span><span class="metric-value" id="sd">检测中</span></div>
        <div class="metric"><span class="metric-label">剩余空间</span><span class="metric-value" id="freeSpace">--</span></div>
        <div class="metric"><span class="metric-label">最新照片</span><span class="metric-value" id="latestImage">--</span></div>
        <div class="metric"><span class="metric-label">本次启动已保存</span><span class="metric-value" id="saved">--</span></div>
        <div class="metric"><span class="metric-label">最近文件大小</span><span class="metric-value" id="bytes">--</span></div>
        <div class="metric"><span class="metric-label">写入耗时</span><span class="metric-value" id="writeMs">--</span></div>
      </section>
    </section>
    <section class="view" id="trendView" role="tabpanel">
      <div class="section-head"><div><h2 class="section-title">每小时客流</h2><div class="section-meta" id="trendDate">今日 00:00–23:59</div></div><button class="tool-button" id="refreshChart" type="button" title="刷新图表" aria-label="刷新图表">↻</button></div>
      <section class="chart-shell"><canvas id="flowChart" aria-label="每小时客流柱状图"></canvas><div class="chart-empty" id="chartEmpty">正在读取客流数据…</div></section>
      <section class="trend-kpis">
        <div class="trend-kpi"><span>今日累计</span><strong id="todayTrend">--</strong></div>
        <div class="trend-kpi"><span>高峰时段</span><strong id="peakHour">--</strong></div>
        <div class="trend-kpi"><span>今日记录</span><strong id="csvRecords">--</strong></div>
      </section>
    </section>
    <section class="view" id="timelapseView" role="tabpanel">
      <div class="section-head"><div><h2 class="section-title">延时摄影回放</h2><div class="section-meta">SD 历史照片 · 基础 5 FPS</div></div><button class="tool-button" id="refreshPhotos" type="button" title="刷新照片范围" aria-label="刷新照片范围">↻</button></div>
      <section class="player-shell" id="playerShell">
        <img id="playbackFrame" alt="延时摄影历史画面">
        <div class="player-placeholder" id="playerPlaceholder">正在读取照片范围…</div>
        <div class="player-badge" id="playbackBadge">5 FPS · 1×</div><div class="player-frame" id="frameNumber">-- / --</div>
      </section>
      <section class="player-controls">
        <div class="transport"><button class="control-button" id="previousFrame" type="button" title="上一帧" aria-label="上一帧">‹</button><button class="control-button primary" id="playButton" type="button" title="播放" aria-label="播放">▶</button><input class="timeline" id="timeline" type="range" min="0" max="0" value="0" aria-label="回放进度"><button class="control-button" id="nextFrame" type="button" title="下一帧" aria-label="下一帧">›</button></div>
        <div class="player-options"><span class="playback-state" id="playbackState">等待照片</span><div class="player-settings"><div class="speed-control" role="group" aria-label="回放速度"><button class="speed-button active" type="button" data-speed="1">1×</button><button class="speed-button" type="button" data-speed="3">3×</button><button class="speed-button" type="button" data-speed="5">5×</button></div><select class="range-select" id="rangeSelect" aria-label="回放范围"><option value="100">最近 100 张</option><option value="300" selected>最近 300 张</option><option value="900">最近 900 张</option><option value="all">全部照片</option></select></div></div>
      </section>
    </section>
    <footer><span id="updatedAt">等待状态更新</span><span id="failureState">运行正常</span></footer>
  </main>
  <script src="/chart.js?v=2"></script>
  <script>
    const el=id=>document.getElementById(id);const stream=el('stream'),placeholder=el('placeholder'),connection=el('connection');let streamRunning=false,userPaused=false,currentView='liveView',lastDeviceCount=-1,flowChart=null;
    function streamUrl(){return 'http://'+location.hostname+':81/stream?t='+Date.now()}
    function startStream(){streamRunning=true;placeholder.textContent='正在载入实时画面…';placeholder.classList.remove('hidden');stream.src=streamUrl();el('pauseButton').textContent='Ⅱ';el('pauseButton').title='暂停预览'}
    function stopStream(manual){streamRunning=false;if(manual)userPaused=true;stream.removeAttribute('src');placeholder.textContent=manual?'预览已暂停':'实时预览已释放';placeholder.classList.remove('hidden');el('pauseButton').textContent='▶';el('pauseButton').title='继续预览'}
    stream.onload=()=>placeholder.classList.add('hidden');stream.onerror=()=>{if(streamRunning){placeholder.textContent='实时画面连接失败';placeholder.classList.remove('hidden')}};
    el('pauseButton').onclick=()=>{if(streamRunning){stopStream(true)}else{userPaused=false;startStream()}};el('fullscreenButton').onclick=()=>{const target=el('cameraShell');if(document.fullscreenElement){document.exitFullscreen()}else if(target.requestFullscreen){target.requestFullscreen()}};
    function formatBytes(value){if(!value)return '--';return value>=1048576?(value/1048576).toFixed(1)+' MB':value>=1024?(value/1024).toFixed(1)+' KB':value+' B'}
    function formatSpace(mb){if(!mb&&mb!==0)return '--';return mb>=1024?(mb/1024).toFixed(1)+' GB':mb+' MB'}
    function setConnection(ok){connection.textContent=ok?'设备在线':'设备离线';connection.classList.toggle('offline',!ok)}
    function activateView(id){currentView=id;document.querySelectorAll('.view').forEach(v=>v.classList.toggle('active',v.id===id));document.querySelectorAll('.tab').forEach(t=>{const active=t.dataset.view===id;t.classList.toggle('active',active);t.setAttribute('aria-selected',active)});if(id==='liveView'){if(!userPaused&&!streamRunning)startStream()}else if(streamRunning){stopStream(false)}if(id==='trendView')loadFlowData();if(id==='timelapseView')loadTimelapseMeta()}
    document.querySelectorAll('.tab').forEach(tab=>tab.onclick=()=>activateView(tab.dataset.view));
    function dateKey(date){return date.getFullYear()+'-'+String(date.getMonth()+1).padStart(2,'0')+'-'+String(date.getDate()).padStart(2,'0')}
    async function loadFlowData(){const empty=el('chartEmpty');empty.textContent='正在读取客流数据…';empty.classList.remove('hidden');try{const response=await fetch('/api/flow.csv?t='+Date.now(),{cache:'no-store'});if(!response.ok)throw new Error();const text=await response.text();const lines=text.trim().split(/\r?\n/).slice(1).filter(Boolean),now=new Date(),currentHour=now.getHours();const buckets=Array(24).fill(0),today=dateKey(now);let todayTotal=0;for(const line of lines){const stamp=line.slice(0,line.lastIndexOf(','));if(!/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}$/.test(stamp))continue;const time=new Date(stamp);if(!Number.isNaN(time.getTime())&&dateKey(time)===today){buckets[time.getHours()]++;todayTotal++}}const visibleBuckets=buckets.slice(0,currentHour+1),labels=visibleBuckets.map((_,i)=>String(i).padStart(2,'0'));el('todayTrend').textContent=todayTotal+' 人次';el('csvRecords').textContent=todayTotal+' 条';const peak=Math.max(...visibleBuckets),peakIndex=visibleBuckets.indexOf(peak);el('peakHour').textContent=peak?String(peakIndex).padStart(2,'0')+':00–'+String(peakIndex).padStart(2,'0')+':59':'--';el('trendDate').textContent=today+' · 00:00–'+String(currentHour).padStart(2,'0')+':59';if(typeof Chart==='undefined')throw new Error();const context=el('flowChart').getContext('2d');if(flowChart)flowChart.destroy();flowChart=new Chart(context,{type:'bar',data:{labels:labels,datasets:[{label:'客流',data:visibleBuckets,backgroundColor:'#157347',borderRadius:3,borderSkipped:false,maxBarThickness:28}]},options:{responsive:true,maintainAspectRatio:false,animation:{duration:350},plugins:{legend:{display:false},tooltip:{callbacks:{title:items=>items[0].label+':00–'+items[0].label+':59',label:item=>'客流 '+item.raw+' 人次'}}},scales:{x:{grid:{display:false},ticks:{color:'#68736d',maxTicksLimit:12}},y:{beginAtZero:true,ticks:{precision:0,color:'#68736d'},grid:{color:'#edf0ee'}}}}});empty.classList.toggle('hidden',todayTotal>0);if(!todayTotal)empty.textContent='当前时段暂无客流记录'}catch(e){empty.textContent='客流数据读取失败';empty.classList.remove('hidden')}}
    el('refreshChart').onclick=loadFlowData;
    const basePlaybackFps=5;let photoMeta=null,playStart=0,playEnd=0,currentFrame=0,playing=false,frameToken=0,playbackSpeed=1;
    function updatePlayerUi(){const total=playEnd>=playStart?playEnd-playStart+1:0,position=total?currentFrame-playStart+1:0;el('frameNumber').textContent=position+' / '+total;el('timeline').value=currentFrame;el('playbackState').textContent=total?('IMG_'+String(currentFrame).padStart(6,'0')):'没有可回放照片'}
    function configureRange(){if(!photoMeta||photoMeta.count===0)return;const selected=el('rangeSelect').value,limit=selected==='all'?photoMeta.count:Number(selected);playEnd=photoMeta.next-1;playStart=Math.max(photoMeta.oldest,photoMeta.next-limit);currentFrame=playStart;el('timeline').min=playStart;el('timeline').max=playEnd;el('timeline').value=currentFrame;pausePlayback();showFrame(currentFrame,false)}
    async function loadTimelapseMeta(){el('playerPlaceholder').textContent='正在读取照片范围…';el('playerPlaceholder').classList.remove('hidden');try{const response=await fetch('/api/timelapse?t='+Date.now(),{cache:'no-store'});if(!response.ok)throw new Error();photoMeta=await response.json();if(!photoMeta.count){el('playerPlaceholder').textContent='暂无延时照片';return}configureRange()}catch(e){el('playerPlaceholder').textContent='照片范围读取失败'}}
    const preloadImages={};function preloadFrames(index){for(let offset=1;offset<=3;offset++){const next=index+offset;if(next<=playEnd&&!preloadImages[next]){const image=new Image();image.src='/api/photo?index='+next;preloadImages[next]=image}}}function showFrame(index,auto){if(index<playStart||index>playEnd){pausePlayback();return}currentFrame=index;updatePlayerUi();preloadFrames(index);const token=++frameToken,started=performance.now(),image=el('playbackFrame');image.onload=()=>{if(token!==frameToken)return;el('playerPlaceholder').classList.add('hidden');if(auto&&playing)setTimeout(()=>showFrame(currentFrame+1,true),Math.max(0,1000/(basePlaybackFps*playbackSpeed)-(performance.now()-started)))};image.onerror=()=>{if(token!==frameToken)return;if(auto&&playing)setTimeout(()=>showFrame(currentFrame+1,true),0);else{el('playerPlaceholder').textContent='该照片不可用';el('playerPlaceholder').classList.remove('hidden')}};image.src='/api/photo?index='+index}
    function playPlayback(){if(!photoMeta||!photoMeta.count)return;if(currentFrame>=playEnd)currentFrame=playStart;playing=true;el('playButton').textContent='Ⅱ';el('playButton').title='暂停';showFrame(currentFrame,true)}
    function pausePlayback(){playing=false;frameToken++;el('playButton').textContent='▶';el('playButton').title='播放'}
    el('playButton').onclick=()=>playing?pausePlayback():playPlayback();el('previousFrame').onclick=()=>{pausePlayback();showFrame(Math.max(playStart,currentFrame-1),false)};el('nextFrame').onclick=()=>{pausePlayback();showFrame(Math.min(playEnd,currentFrame+1),false)};el('timeline').oninput=e=>{pausePlayback();showFrame(Number(e.target.value),false)};el('rangeSelect').onchange=configureRange;el('refreshPhotos').onclick=loadTimelapseMeta;document.querySelectorAll('.speed-button').forEach(button=>button.onclick=()=>{playbackSpeed=Number(button.dataset.speed);document.querySelectorAll('.speed-button').forEach(item=>item.classList.toggle('active',item===button));el('playbackBadge').textContent=(basePlaybackFps*playbackSpeed)+' FPS · '+playbackSpeed+'×'});
    async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error();const s=await r.json();setConnection(true);
      el('sd').textContent=s.sd?'就绪':'初始化中';el('sd').className='metric-value '+(s.sd?'good':'bad');el('count').textContent=s.count;
      const score=Number(s.motionScore)||0;el('motionState').textContent=!s.armed?'\u6b63\u5728\u6821\u51c6':(s.near?'\u5df2\u5230\u8fd1\u5904\uff0c\u7b49\u5f85\u79bb\u5f00':(s.approach?'\u76ee\u6807\u6b63\u5728\u9760\u8fd1':(s.motion?'\u68c0\u6d4b\u5230\u79fb\u52a8':'\u53ef\u4ee5\u8ba1\u6570')));el('motionState').classList.toggle('active',s.armed&&s.motion);el('motionScore').textContent=s.armed?score.toFixed(2)+'%':'--%';el('motionBar').style.width=s.armed?Math.min(100,score)+'%':'0';el('motionBar').classList.toggle('active',s.armed&&s.motion);
      el('saved').textContent=s.saved+' 张';el('bytes').textContent=formatBytes(s.lastBytes);el('writeMs').textContent=s.lastWriteMs+' ms';el('freeSpace').textContent=formatSpace(s.freeMB);el('latestImage').textContent=s.nextImage>1?'IMG_'+String(s.nextImage-1).padStart(6,'0'): '--';
      el('updatedAt').textContent='更新于 '+new Date().toLocaleTimeString('zh-CN',{hour12:false});el('failureState').textContent=s.failures?('写入异常 '+s.failures+' 次'):'运行正常';if(lastDeviceCount!==s.count){lastDeviceCount=s.count;if(currentView==='trendView')loadFlowData()}
    }catch(e){setConnection(false);el('updatedAt').textContent='状态更新失败'}}
    fetch('/api/time?epoch='+Math.floor(Date.now()/1000),{method:'POST'}).catch(()=>{});startStream();refresh();setInterval(refresh,2000);
  </script>
</body>
</html>
)HTML";

const char kNetworkHtml[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32-CAM Wi-Fi</title>
<style>body{font-family:Arial,"Microsoft YaHei",sans-serif;background:#f4f6f5;color:#17201b;margin:0;padding:24px}main{max-width:560px;margin:auto;background:#fff;border:1px solid #dfe5e1;border-radius:8px;padding:22px}h1{font-size:21px;margin:0 0 8px}p{color:#68736d;font-size:14px;line-height:1.6}label{display:block;font-size:13px;font-weight:700;margin:16px 0 6px}input,select{width:100%;box-sizing:border-box;padding:11px;border:1px solid #cbd5cf;border-radius:5px;font-size:15px;background:#fff}button{margin-top:12px;padding:11px 16px;border:0;border-radius:5px;background:#157347;color:#fff;font-weight:700;font-size:14px}button.secondary{background:#e9efeb;color:#195536;margin-left:8px}a{color:#157347}.status{margin-top:18px;padding:12px;background:#f4f6f5;border-radius:5px;font-size:13px;white-space:pre-line}.hidden{display:none}.restart{position:fixed;inset:0;z-index:20;background:rgba(10,24,17,.88);display:flex;align-items:center;justify-content:center;padding:22px;box-sizing:border-box}.restart.hidden{display:none}.restart-panel{width:min(420px,100%);background:#fff;border-radius:8px;padding:28px;text-align:center;box-sizing:border-box}.restart-panel h2{font-size:23px;margin:14px 0 8px}.restart-panel p{margin:8px 0}.restart-panel strong{color:#157347}.spinner{width:42px;height:42px;margin:auto;border:5px solid #dfe8e1;border-top-color:#157347;border-radius:50%;animation:spin .9s linear infinite}@keyframes spin{to{transform:rotate(360deg)}}</style></head>
<body><main><h1>连接店铺 Wi-Fi</h1><p>请选择附近的 2.4 GHz Wi-Fi，输入密码后保存。设备热点会一直保留，联网失败时仍可重新进入本页面。</p>
<label for="ssid">附近的 Wi-Fi</label><select id="ssid"><option value="">正在扫描...</option></select><button id="scan" class="secondary" type="button">重新扫描</button>
<input id="customSsid" class="hidden" maxlength="32" autocomplete="off" placeholder="输入隐藏 Wi-Fi 名称">
<label for="password">Wi-Fi 密码</label><input id="password" type="password" maxlength="63" autocomplete="off" placeholder="请输入密码">
<button id="save" type="button">保存并连接</button><div class="status" id="status">正在读取网络状态...</div><p><a href="/">返回监控页面</a></p></main><div class="restart hidden" id="restart"><div class="restart-panel"><div class="spinner"></div><h2>设备正在重启</h2><p id="restartText">正在保存 Wi-Fi 设置...</p><p>网页和热点暂时断开属于正常现象，请不要关闭 ESP32 电源。</p></div></div>
<script>const statusEl=document.getElementById('status'),ssidEl=document.getElementById('ssid'),customEl=document.getElementById('customSsid'),restartEl=document.getElementById('restart'),restartText=document.getElementById('restartText');let configuredSsid='';function updateCustom(){customEl.classList.toggle('hidden',ssidEl.value!=='__manual__');}function showRestart(ssid){restartEl.classList.remove('hidden');document.querySelectorAll('button,input,select').forEach(item=>item.disabled=true);let seconds=15;restartText.innerHTML='正在连接 <strong>'+ssid.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))+'</strong><br>预计还需 '+seconds+' 秒';const timer=setInterval(()=>{seconds--;if(seconds>0)restartText.innerHTML='正在连接 <strong>'+ssid.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))+'</strong><br>预计还需 '+seconds+' 秒';else{clearInterval(timer);restartText.innerHTML='重启已完成。请让手机连接 <strong>'+ssid.replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))+'</strong>，再访问 esp32cam.local';}},1000);}async function scan(){statusEl.textContent='正在扫描附近 Wi-Fi...';try{const r=await fetch('/api/network/scan?t='+Date.now(),{cache:'no-store'});if(!r.ok)throw new Error();const data=await r.json();ssidEl.innerHTML='';const seen=new Set();(data.networks||[]).forEach(n=>{if(!n.ssid||seen.has(n.ssid))return;seen.add(n.ssid);const option=document.createElement('option');option.value=n.ssid;option.textContent=n.ssid+'  ('+n.rssi+' dBm'+(n.secure?' · 有密码':' · 开放')+')';ssidEl.appendChild(option);});const manual=document.createElement('option');manual.value='__manual__';manual.textContent='其他/隐藏 Wi-Fi（手动输入）';ssidEl.appendChild(manual);if(configuredSsid&&seen.has(configuredSsid))ssidEl.value=configuredSsid;else if(configuredSsid){ssidEl.value='__manual__';customEl.value=configuredSsid;}updateCustom();statusEl.textContent='扫描到 '+seen.size+' 个 Wi-Fi，请选择后输入密码。';}catch(e){statusEl.textContent='扫描失败，请确认手机仍连接 ESP32-CAM-DAY1；也可以选择手动输入。';ssidEl.innerHTML='<option value="__manual__">其他/隐藏 Wi-Fi（手动输入）</option>';ssidEl.value='__manual__';updateCustom();}}ssidEl.onchange=updateCustom;document.getElementById('scan').onclick=scan;document.getElementById('save').onclick=async()=>{const ssid=(ssidEl.value==='__manual__'?customEl.value:ssidEl.value).trim(),password=document.getElementById('password').value;if(!ssid){statusEl.textContent='请先选择或输入 Wi-Fi 名称';return;}showRestart(ssid);const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password);try{await fetch('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});}catch(e){}};async function load(){try{const r=await fetch('/api/network',{cache:'no-store'}),s=await r.json();configuredSsid=s.configuredSsid||'';statusEl.textContent='当前状态：'+(s.connected?'已连接':'未连接')+'\nSSID：'+(s.ssid||'--')+'\nLAN IP：'+(s.ip||'--');}catch(e){statusEl.textContent='正在读取网络状态...';}scan();}load();</script></body></html>
)HTML";

bool initializeCamera() {
  camera_config_t config{};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = kPinY2;
  config.pin_d1 = kPinY3;
  config.pin_d2 = kPinY4;
  config.pin_d3 = kPinY5;
  config.pin_d4 = kPinY6;
  config.pin_d5 = kPinY7;
  config.pin_d6 = kPinY8;
  config.pin_d7 = kPinY9;
  config.pin_xclk = kPinXclk;
  config.pin_pclk = kPinPclk;
  config.pin_vsync = kPinVsync;
  config.pin_href = kPinHref;
  config.pin_sccb_sda = kPinSiod;
  config.pin_sccb_scl = kPinSioc;
  config.pin_pwdn = kPinPwdn;
  config.pin_reset = kPinReset;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  // A single frame buffer avoids alternating decoded frames observed with
  // this OV2640 module while snapshot and motion tasks share the camera.
  config.fb_count = 1;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", error);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_framesize(sensor, FRAMESIZE_VGA);
  }

  Serial.printf("[CAM] ready: PID=0x%04x VGA JPEG, PSRAM=%s, buffers=%d\n",
                sensor == nullptr ? 0 : sensor->id.PID,
                psramFound() ? "YES" : "NO", config.fb_count);
  return true;
}

void formatHourDirectory(char *path, size_t pathSize, uint32_t index) {
  const uint32_t hourIndex = (index - 1) / kCapturesPerHour;
  std::snprintf(path, pathSize, "%s/H%07lu", kTimelapseDir,
                static_cast<unsigned long>(hourIndex));
}

void formatImagePath(char *path, size_t pathSize, uint32_t index) {
  char directory[48];
  formatHourDirectory(directory, sizeof(directory), index);
  std::snprintf(path, pathSize, "%s/IMG_%06lu.jpg", directory,
                static_cast<unsigned long>(index));
}

bool formatDatedImagePath(char *path, size_t pathSize, int64_t epoch) {
  const time_t captureTime = static_cast<time_t>(epoch);
  if (captureTime < 1700000000) {
    return false;
  }
  struct tm localTime {};
  localtime_r(&captureTime, &localTime);
  const int length = std::snprintf(
      path, pathSize,
      "%s/%04d%02d%02d%02d/%02d%02d%02d.jpg",
      kTimelapseDir, localTime.tm_year + 1900, localTime.tm_mon + 1,
      localTime.tm_mday, localTime.tm_hour, localTime.tm_hour,
      localTime.tm_min, localTime.tm_sec);
  return length > 0 && static_cast<size_t>(length) < pathSize;
}

bool formatLegacyDatedImagePath(char *path, size_t pathSize, int64_t epoch) {
  const time_t captureTime = static_cast<time_t>(epoch);
  if (captureTime < 1700000000) {
    return false;
  }
  struct tm localTime {};
  localtime_r(&captureTime, &localTime);
  const int length = std::snprintf(
      path, pathSize,
      "%s/%04d%02d%02d%02d/%04d%02d%02d%02d%02d%02d.jpg",
      kTimelapseDir, localTime.tm_year + 1900, localTime.tm_mon + 1,
      localTime.tm_mday, localTime.tm_hour, localTime.tm_year + 1900,
      localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour,
      localTime.tm_min, localTime.tm_sec);
  return length > 0 && static_cast<size_t>(length) < pathSize;
}

bool readCatalogRecord(File &catalog, size_t recordNumber,
                       PhotoCatalogRecord *record) {
  const size_t offset = recordNumber * sizeof(PhotoCatalogRecord);
  return catalog.seek(offset) &&
         catalog.read(reinterpret_cast<uint8_t *>(record), sizeof(*record)) ==
             sizeof(*record);
}

bool findCatalogEpoch(uint32_t index, int64_t *epoch) {
  File catalog = SD_MMC.open(kPhotoCatalogPath, FILE_READ);
  if (!catalog) {
    return false;
  }
  const size_t recordCount = catalog.size() / sizeof(PhotoCatalogRecord);
  size_t lower = 0;
  size_t upper = recordCount;
  PhotoCatalogRecord record {};
  while (lower < upper) {
    const size_t middle = lower + (upper - lower) / 2;
    if (!readCatalogRecord(catalog, middle, &record)) {
      catalog.close();
      return false;
    }
    if (record.index < index) {
      lower = middle + 1;
    } else {
      upper = middle;
    }
  }
  const bool found = lower < recordCount &&
                     readCatalogRecord(catalog, lower, &record) &&
                     record.index == index;
  catalog.close();
  if (found) {
    *epoch = record.epoch;
  }
  return found;
}

bool appendCatalogRecord(uint32_t index, int64_t epoch) {
  File catalog = SD_MMC.open(kPhotoCatalogPath, FILE_APPEND);
  if (!catalog) {
    Serial.println("[STORAGE] cannot append photo catalog");
    return false;
  }
  const PhotoCatalogRecord record = {.index = index, .epoch = epoch};
  const size_t written = catalog.write(
      reinterpret_cast<const uint8_t *>(&record), sizeof(record));
  catalog.flush();
  catalog.close();
  if (written != sizeof(record)) {
    Serial.println("[STORAGE] short photo catalog write");
    return false;
  }
  return true;
}

void formatLegacyImagePath(char *path, size_t pathSize, uint32_t index) {
  std::snprintf(path, pathSize, "%s/IMG_%06lu.jpg", kTimelapseDir,
                static_cast<unsigned long>(index));
}

bool resolveImagePath(uint32_t index, char *path, size_t pathSize,
                      bool *hourlyPath = nullptr) {
  int64_t epoch = 0;
  if (findCatalogEpoch(index, &epoch) &&
      formatDatedImagePath(path, pathSize, epoch) && SD_MMC.exists(path)) {
    if (hourlyPath != nullptr) {
      *hourlyPath = true;
    }
    return true;
  }
  if (epoch >= 1700000000 &&
      formatLegacyDatedImagePath(path, pathSize, epoch) &&
      SD_MMC.exists(path)) {
    if (hourlyPath != nullptr) {
      *hourlyPath = true;
    }
    return true;
  }

  formatImagePath(path, pathSize, index);
  if (SD_MMC.exists(path)) {
    if (hourlyPath != nullptr) {
      *hourlyPath = true;
    }
    return true;
  }

  formatLegacyImagePath(path, pathSize, index);
  if (SD_MMC.exists(path)) {
    if (hourlyPath != nullptr) {
      *hourlyPath = false;
    }
    return true;
  }
  return false;
}

bool imageExists(uint32_t index) {
  char path[64];
  return resolveImagePath(index, path, sizeof(path));
}

uint32_t findNextImageIndex() {

  if (!imageExists(1)) {
    return 1;
  }

  uint32_t lower = 1;
  uint32_t upper = 2;
  while (upper < 1000000 && imageExists(upper)) {
    lower = upper;
    upper *= 2;
  }
  if (upper > 1000000) {
    upper = 1000000;
  }

  while (lower + 1 < upper) {
    const uint32_t middle = lower + (upper - lower) / 2;
    if (imageExists(middle)) {
      lower = middle;
    } else {
      upper = middle;
    }
  }
  return upper;
}

void scanDirectoryImages(File &directory, uint32_t *minimum, uint32_t *maximum) {
  for (File entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (entry.isDirectory()) {
      scanDirectoryImages(entry, minimum, maximum);
    } else {
      const char *fullName = entry.name();
      const char *baseName = std::strrchr(fullName, '/');
      baseName = baseName == nullptr ? fullName : baseName + 1;
      unsigned int index = 0;
      if (std::sscanf(baseName, "IMG_%06u.jpg", &index) == 1 && index != 0) {
        *minimum = std::min(*minimum, static_cast<uint32_t>(index));
        *maximum = std::max(*maximum, static_cast<uint32_t>(index));
      }
    }
    entry.close();
  }
}

void scanCatalogImages(uint32_t *minimum, uint32_t *maximum) {
  File catalog = SD_MMC.open(kPhotoCatalogPath, FILE_READ);
  if (!catalog) {
    return;
  }
  const size_t recordCount = catalog.size() / sizeof(PhotoCatalogRecord);
  PhotoCatalogRecord record {};
  char path[64];
  for (size_t i = 0; i < recordCount; ++i) {
    if (!readCatalogRecord(catalog, i, &record)) {
      break;
    }
    const bool currentPathExists =
        formatDatedImagePath(path, sizeof(path), record.epoch) &&
        SD_MMC.exists(path);
    const bool legacyPathExists =
        !currentPathExists &&
        formatLegacyDatedImagePath(path, sizeof(path), record.epoch) &&
        SD_MMC.exists(path);
    if (record.index != 0 && (currentPathExists || legacyPathExists)) {
      *minimum = std::min(*minimum, record.index);
      *maximum = std::max(*maximum, record.index);
    }
  }
  catalog.close();
}

bool scanImageRange(uint32_t *oldest, uint32_t *next) {
  File directory = SD_MMC.open(kTimelapseDir);
  if (!directory || !directory.isDirectory()) {
    return false;
  }

  uint32_t minimum = UINT32_MAX;
  uint32_t maximum = 0;
  scanDirectoryImages(directory, &minimum, &maximum);
  directory.close();
  scanCatalogImages(&minimum, &maximum);

  *oldest = maximum == 0 ? 1 : minimum;
  *next = maximum + 1;
  return true;
}

bool persistCaptureState(uint32_t next, uint32_t oldest) {
  SD_MMC.remove(kCaptureStateTempPath);
  File output = SD_MMC.open(kCaptureStateTempPath, FILE_WRITE);
  if (!output) {
    Serial.println("[STORAGE] cannot write capture state");
    return false;
  }
  output.printf("%lu,%lu\n", static_cast<unsigned long>(next),
                static_cast<unsigned long>(oldest));
  output.flush();
  output.close();

  SD_MMC.remove(kCaptureStatePath);
  if (!SD_MMC.rename(kCaptureStateTempPath, kCaptureStatePath)) {
    SD_MMC.remove(kCaptureStateTempPath);
    Serial.println("[STORAGE] cannot commit capture state");
    return false;
  }
  return true;
}

bool loadCaptureState(uint32_t *next, uint32_t *oldest) {
  File input = SD_MMC.open(kCaptureStatePath, FILE_READ);
  if (!input) {
    return false;
  }

  char line[64];
  const size_t length = input.readBytesUntil('\n', line, sizeof(line) - 1);
  input.close();
  line[length] = '\0';

  unsigned int parsedNext = 0;
  unsigned int parsedOldest = 0;
  if (std::sscanf(line, "%u,%u", &parsedNext, &parsedOldest) != 2 ||
      parsedNext == 0 || parsedOldest == 0 || parsedOldest > parsedNext) {
    Serial.println("[STORAGE] capture state invalid; rebuilding");
    return false;
  }
  *next = static_cast<uint32_t>(parsedNext);
  *oldest = static_cast<uint32_t>(parsedOldest);
  return true;
}

bool initializeCaptureState() {
  uint32_t next = 1;
  uint32_t oldest = 1;
  if (loadCaptureState(&next, &oldest)) {
    while (imageExists(next)) {
      ++next;
    }
    if (oldest < next && !imageExists(oldest)) {
      oldest = std::min(next, std::max(oldest, kPhotoRecoveryOldestHint));
      Serial.printf(
          "[STORAGE] leading photo gap detected; using recovery floor %06lu\n",
          static_cast<unsigned long>(oldest));
    }
  } else if (imageExists(1)) {
    next = findNextImageIndex();
  } else if (!scanImageRange(&oldest, &next)) {
    Serial.println("[STORAGE] cannot scan /timelapse");
    return false;
  }

  nextImageIndex = next;
  oldestImageIndex = oldest;
  if (!persistCaptureState(next, oldest)) {
    return false;
  }
  Serial.printf("[STORAGE] state: oldest=%06lu next=%06lu\n",
                static_cast<unsigned long>(oldest),
                static_cast<unsigned long>(next));
  return true;
}

uint32_t currentLocalDate() {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    return 0;
  }
  struct tm localTime {};
  localtime_r(&now, &localTime);
  return static_cast<uint32_t>(localTime.tm_year + 1900) * 10000UL +
         static_cast<uint32_t>(localTime.tm_mon + 1) * 100UL +
         static_cast<uint32_t>(localTime.tm_mday);
}

uint32_t loadCustomerCountForDate(uint32_t date) {
  File input = SD_MMC.open(kFlowCsvPath, FILE_READ);
  if (!input) {
    return 0;
  }

  char datePrefix[11];
  std::snprintf(datePrefix, sizeof(datePrefix), "%04lu-%02lu-%02lu",
                static_cast<unsigned long>(date / 10000UL),
                static_cast<unsigned long>((date / 100UL) % 100UL),
                static_cast<unsigned long>(date % 100UL));
  uint32_t eventCount = 0;
  char line[96];
  size_t length = 0;
  while (input.available()) {
    const int value = input.read();
    if (value == '\r') {
      continue;
    }
    if (value == '\n' || length == sizeof(line) - 1) {
      line[length] = '\0';
      if (std::strncmp(line, datePrefix, 10) == 0) {
        const char *comma = std::strrchr(line, ',');
        if (comma != nullptr && comma[1] >= '0' && comma[1] <= '9') {
          ++eventCount;
        }
      }
      length = 0;
    } else {
      line[length++] = static_cast<char>(value);
    }
  }
  if (length != 0) {
    line[length] = '\0';
    if (std::strncmp(line, datePrefix, 10) == 0) {
      const char *comma = std::strrchr(line, ',');
      if (comma != nullptr && comma[1] >= '0' && comma[1] <= '9') {
        ++eventCount;
      }
    }
  }
  input.close();
  return eventCount;
}

void updateDailyCustomerCountLocked() {
  const uint32_t today = currentLocalDate();
  if (today == 0 || today == customerCountDate) {
    return;
  }

  if (customerCountDate == 0) {
    const uint32_t unsynchronizedCount = customerCount;
    const uint32_t restoredCount = loadCustomerCountForDate(today);
    customerCount = restoredCount + unsynchronizedCount;
    customerCountDate = today;
    Serial.printf("[FLOW] date synchronized: %08lu, restored=%lu, unsynced=%lu\n",
                  static_cast<unsigned long>(today),
                  static_cast<unsigned long>(restoredCount),
                  static_cast<unsigned long>(unsynchronizedCount));
    return;
  }

  const uint32_t previousDate = customerCountDate;
  customerCount = 0;
  customerCountDate = today;
  Serial.printf("[FLOW] midnight reset: %08lu -> %08lu, count=0\n",
                static_cast<unsigned long>(previousDate),
                static_cast<unsigned long>(today));
}

void maintainDailyCustomerCount() {
  if (!sdReady || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return;
  }
  updateDailyCustomerCountLocked();
  xSemaphoreGive(sdMutex);
}

bool initializeFlowCsv() {
  if (!SD_MMC.exists(kDataDir) && !SD_MMC.mkdir(kDataDir)) {
    Serial.println("[FLOW] cannot create /data");
    return false;
  }
  if (!SD_MMC.exists(kFlowCsvPath)) {
    File output = SD_MMC.open(kFlowCsvPath, FILE_WRITE);
    if (!output) {
      Serial.println("[FLOW] cannot create /data/flow.csv");
      return false;
    }
    output.println("timestamp,count");
    output.flush();
    output.close();
  }
  customerCount = 0;
  customerCountDate = 0;
  Serial.printf("[FLOW] ready: %s, waiting for clock synchronization\n",
                kFlowCsvPath);
  return true;
}

bool removeStorageTree(const char *path, uint32_t *removedFiles) {
  File directory = SD_MMC.open(path, FILE_READ);
  if (!directory) {
    return !SD_MMC.exists(path);
  }
  if (!directory.isDirectory()) {
    directory.close();
    if (!SD_MMC.remove(path)) {
      Serial.printf("[RESET] cannot delete file: %s\n", path);
      return false;
    }
    ++*removedFiles;
    return true;
  }

  bool success = true;
  while (true) {
    File entry = directory.openNextFile();
    if (!entry) {
      break;
    }
    char childPath[128];
    const char *entryName = entry.name();
    if (entryName[0] == '/') {
      std::snprintf(childPath, sizeof(childPath), "%s", entryName);
    } else {
      std::snprintf(childPath, sizeof(childPath), "%s/%s", path, entryName);
    }
    const bool childIsDirectory = entry.isDirectory();
    entry.close();

    if (childIsDirectory) {
      success = removeStorageTree(childPath, removedFiles) && success;
    } else if (SD_MMC.remove(childPath)) {
      ++*removedFiles;
      if ((*removedFiles % 250) == 0) {
        Serial.printf("[RESET] deleted %lu files\n",
                      static_cast<unsigned long>(*removedFiles));
      }
    } else {
      Serial.printf("[RESET] cannot delete file: %s\n", childPath);
      success = false;
    }
    delay(1);
  }
  directory.close();

  if (!SD_MMC.rmdir(path) && SD_MMC.exists(path)) {
    Serial.printf("[RESET] cannot delete directory: %s\n", path);
    success = false;
  }
  return success;
}

bool resetStoredRecordsOnce() {
  if (!kRunPhotoResetOnBoot) {
    return true;
  }
  if (SD_MMC.exists(kStorageResetMarkerPath)) {
    return true;
  }

  Serial.println("[RESET] clearing photos and photo indexes; preserving flow.csv");
  uint32_t removedFiles = 0;
  const bool photosRemoved = removeStorageTree(kTimelapseDir, &removedFiles);
  if (!photosRemoved) {
    Serial.printf("[RESET] incomplete after %lu files; will retry after reboot\n",
                  static_cast<unsigned long>(removedFiles));
    return false;
  }

  SD_MMC.remove(kPhotoCatalogPath);
  SD_MMC.remove(kCaptureStatePath);
  SD_MMC.remove(kCaptureStateTempPath);

  File marker = SD_MMC.open(kStorageResetMarkerPath, FILE_WRITE);
  if (!marker) {
    Serial.println("[RESET] cannot create completion marker");
    return false;
  }
  marker.println("photos cleared 2026-08-06; flow.csv preserved");
  marker.flush();
  marker.close();

  nextImageIndex = 1;
  oldestImageIndex = 1;
  savedThisBoot = 0;
  captureFailures = 0;
  lastImageBytes = 0;
  lastWriteMs = 0;
  Serial.printf("[RESET] photo cleanup complete: deleted %lu files\n",
                static_cast<unsigned long>(removedFiles));
  return true;
}

bool initializeSdCard() {
  // One-bit SD mode avoids the GPIO 4 flash LED conflict.
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] mount failed; check card seating and FAT32 format");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] no card detected");
    return false;
  }
  if (!resetStoredRecordsOnce()) {
    return false;
  }
  if (!SD_MMC.exists(kTimelapseDir) && !SD_MMC.mkdir(kTimelapseDir)) {
    Serial.println("[SD] cannot create /timelapse");
    return false;
  }
  if (!initializeFlowCsv()) {
    return false;
  }

  if (!initializeCaptureState()) {
    return false;
  }
  const uint64_t totalBytes = SD_MMC.totalBytes();
  const uint64_t usedBytes = SD_MMC.usedBytes();
  estimatedFreeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
  freeSpaceMb = static_cast<uint32_t>(estimatedFreeBytes / (1024ULL * 1024ULL));
  sdReady = true;
  Serial.printf("[SD] ready: %llu MB, free=%llu MB, oldest=%06lu next=%06lu\n",
                SD_MMC.cardSize() / (1024ULL * 1024ULL),
                estimatedFreeBytes / (1024ULL * 1024ULL),
                static_cast<unsigned long>(oldestImageIndex),
                static_cast<unsigned long>(nextImageIndex));
  return true;
}

bool removeOldestImage(uint32_t next, uint64_t *removedBytes) {
  *removedBytes = 0;
  while (oldestImageIndex < next) {
    const uint32_t index = oldestImageIndex++;
    char path[64];
    bool hourlyPath = false;
    if (!resolveImagePath(index, path, sizeof(path), &hourlyPath)) {
      continue;
    }
    File input = SD_MMC.open(path, FILE_READ);
    if (!input) {
      continue;
    }
    const uint64_t fileSize = input.size();
    input.close();
    if (!SD_MMC.remove(path)) {
      Serial.printf("[STORAGE] cannot delete oldest photo: %s\n", path);
      return false;
    }
    *removedBytes = fileSize;
    Serial.printf("[STORAGE] full: deleted %s (%llu bytes)\n", path,
                  fileSize);
    if (hourlyPath) {
      char directory[64];
      std::snprintf(directory, sizeof(directory), "%s", path);
      char *separator = std::strrchr(directory, '/');
      if (separator != nullptr) {
        *separator = '\0';
        SD_MMC.rmdir(directory);
      }
    }
    return true;
  }
  Serial.println("[STORAGE] no timelapse photo available to delete");
  return false;
}

bool ensureStorageSpace(size_t jpegLength, uint32_t next) {
  const uint64_t requiredBytes = kStorageReserveBytes + jpegLength;

  while (estimatedFreeBytes < requiredBytes) {
    uint64_t removedBytes = 0;
    if (!removeOldestImage(next, &removedBytes)) {
      return false;
    }
    estimatedFreeBytes += removedBytes;
    freeSpaceMb = static_cast<uint32_t>(estimatedFreeBytes / (1024ULL * 1024ULL));
  }
  return true;
}

bool saveJpeg(const uint8_t *jpeg, size_t jpegLength, uint32_t imageIndex) {
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.println("[SHOT] SD lock timeout");
    return false;
  }

  char finalPath[64];
  char temporaryPath[64];
  char hourDirectory[64];
  const time_t captureTime = time(nullptr);
  const bool timestamped = captureTime >= 1700000000 &&
                           formatDatedImagePath(finalPath, sizeof(finalPath),
                                                static_cast<int64_t>(captureTime));
  if (!timestamped) {
    formatImagePath(finalPath, sizeof(finalPath), imageIndex);
  }
  std::snprintf(hourDirectory, sizeof(hourDirectory), "%s", finalPath);
  char *separator = std::strrchr(hourDirectory, '/');
  if (separator == nullptr) {
    Serial.println("[SHOT] invalid image path");
    xSemaphoreGive(sdMutex);
    return false;
  }
  *separator = '\0';
  std::snprintf(temporaryPath, sizeof(temporaryPath), "%s/.capture.tmp",
                hourDirectory);

  if (!SD_MMC.exists(hourDirectory) && !SD_MMC.mkdir(hourDirectory)) {
    Serial.printf("[SHOT] cannot create hourly directory: %s\n", hourDirectory);
    xSemaphoreGive(sdMutex);
    return false;
  }

  if (imageExists(imageIndex) || SD_MMC.exists(finalPath)) {
    Serial.printf("[SHOT] refusing to overwrite: %s\n", finalPath);
    xSemaphoreGive(sdMutex);
    return false;
  }

  SD_MMC.remove(temporaryPath);
  const size_t storageRequired =
      jpegLength + (timestamped ? sizeof(PhotoCatalogRecord) : 0);
  if (!ensureStorageSpace(storageRequired, imageIndex)) {
    Serial.println("[SHOT] not enough SD space after cleanup");
    xSemaphoreGive(sdMutex);
    return false;
  }
  File output = SD_MMC.open(temporaryPath, FILE_WRITE);
  if (!output) {
    Serial.printf("[SHOT] open failed: %s\n", temporaryPath);
    xSemaphoreGive(sdMutex);
    return false;
  }

  const size_t written = output.write(jpeg, jpegLength);
  output.flush();
  output.close();
  if (written != jpegLength) {
    SD_MMC.remove(temporaryPath);
    Serial.printf("[SHOT] short write: %u/%u bytes\n",
                  static_cast<unsigned int>(written),
                  static_cast<unsigned int>(jpegLength));
    xSemaphoreGive(sdMutex);
    return false;
  }

  if (!SD_MMC.rename(temporaryPath, finalPath)) {
    SD_MMC.remove(temporaryPath);
    Serial.printf("[SHOT] rename failed: %s\n", finalPath);
    xSemaphoreGive(sdMutex);
    return false;
  }

  if (timestamped &&
      !appendCatalogRecord(imageIndex, static_cast<int64_t>(captureTime))) {
    SD_MMC.remove(finalPath);
    SD_MMC.rmdir(hourDirectory);
    Serial.printf("[SHOT] rolled back image without catalog: %s\n", finalPath);
    xSemaphoreGive(sdMutex);
    return false;
  }

  Serial.printf("[SHOT] %s, %u bytes\n", finalPath,
                 static_cast<unsigned int>(jpegLength));
  estimatedFreeBytes = estimatedFreeBytes > storageRequired
                           ? estimatedFreeBytes - storageRequired
                           : 0;
  freeSpaceMb = static_cast<uint32_t>(estimatedFreeBytes / (1024ULL * 1024ULL));
  if (++captureStateChanges >= kCaptureStateCheckpointImages &&
      persistCaptureState(imageIndex + 1, oldestImageIndex)) {
    captureStateChanges = 0;
  }
  xSemaphoreGive(sdMutex);
  return true;
}

bool captureJpeg(framesize_t requestedFrameSize, uint8_t **jpeg,
                 size_t *jpegLength) {
  *jpeg = nullptr;
  *jpegLength = 0;
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(4000)) != pdTRUE) {
    return false;
  }

  if (activeFrameSize != requestedFrameSize) {
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor == nullptr ||
        sensor->set_framesize(sensor, requestedFrameSize) != 0) {
      xSemaphoreGive(cameraMutex);
      Serial.println("[CAM] frame-size switch failed");
      return false;
    }
    activeFrameSize = requestedFrameSize;
    camera_fb_t *staleFrame = esp_camera_fb_get();
    if (staleFrame != nullptr) {
      esp_camera_fb_return(staleFrame);
    }
  }

  camera_fb_t *frame = esp_camera_fb_get();
  bool converted = false;
  if (frame != nullptr) {
    const bool expectedDimensions =
        (requestedFrameSize == FRAMESIZE_VGA && frame->width == 640 &&
         frame->height == 480) ||
        (requestedFrameSize == FRAMESIZE_XGA && frame->width == 1024 &&
         frame->height == 768);
    if (!expectedDimensions) {
      Serial.printf("[CAM] unexpected frame: %ux%u\n", frame->width,
                    frame->height);
    } else if (frame->format == PIXFORMAT_JPEG) {
      *jpeg = static_cast<uint8_t *>(malloc(frame->len));
      if (*jpeg != nullptr) {
        std::memcpy(*jpeg, frame->buf, frame->len);
        *jpegLength = frame->len;
        converted = true;
      }
    } else {
      converted = frame2jpg(frame, kSoftwareJpegQuality, jpeg, jpegLength);
    }
    esp_camera_fb_return(frame);
  }
  xSemaphoreGive(cameraMutex);
  return converted;
}

bool lockCameraAutoControls() {
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[CAM] auto-control lock skipped: camera busy");
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  int result = -1;
  if (sensor != nullptr) {
    result = sensor->set_exposure_ctrl(sensor, 0);
    result |= sensor->set_gain_ctrl(sensor, 0);
    result |= sensor->set_whitebal(sensor, 0);
    result |= sensor->set_awb_gain(sensor, 0);
  }
  xSemaphoreGive(cameraMutex);

  Serial.printf("[CAM] exposure/gain/white-balance lock: %s\n",
                result == 0 ? "OK" : "FAILED");
  return result == 0;
}

bool enableCameraAutoControls() {
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("[CAM] auto-control enable skipped: camera busy");
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  int result = -1;
  if (sensor != nullptr) {
    result = sensor->set_exposure_ctrl(sensor, 1);
    result |= sensor->set_gain_ctrl(sensor, 1);
    result |= sensor->set_whitebal(sensor, 1);
    result |= sensor->set_awb_gain(sensor, 1);
  }
  xSemaphoreGive(cameraMutex);

  Serial.printf("[CAM] automatic exposure/gain/white-balance: %s\n",
                result == 0 ? "ON" : "FAILED");
  return result == 0;
}

void formatFlowTimestamp(char *buffer, size_t bufferSize) {
  const time_t now = time(nullptr);
  if (now >= 1700000000) {
    struct tm localTime {};
    localtime_r(&now, &localTime);
    std::strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%S", &localTime);
  } else {
    std::snprintf(buffer, bufferSize, "uptime-%010lu",
                  static_cast<unsigned long>(millis() / 1000));
  }
}

bool appendFlowEvent() {
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(kFlowSdLockTimeoutMs)) != pdTRUE) {
    Serial.println("[FLOW] SD lock timeout");
    return false;
  }

  updateDailyCustomerCountLocked();
  const uint32_t newCount = customerCount + 1;

  File output = SD_MMC.open(kFlowCsvPath, FILE_APPEND);
  if (!output) {
    xSemaphoreGive(sdMutex);
    Serial.println("[FLOW] cannot append flow.csv");
    return false;
  }

  char timestamp[32];
  char row[64];
  formatFlowTimestamp(timestamp, sizeof(timestamp));
  const int rowLength = std::snprintf(row, sizeof(row), "%s,%lu\n", timestamp,
                                      static_cast<unsigned long>(newCount));
  const size_t written = output.write(
      reinterpret_cast<const uint8_t *>(row), static_cast<size_t>(rowLength));
  output.flush();
  output.close();
  if (written == static_cast<size_t>(rowLength)) {
    estimatedFreeBytes = estimatedFreeBytes > written
                             ? estimatedFreeBytes - written
                             : 0;
    freeSpaceMb = static_cast<uint32_t>(estimatedFreeBytes / (1024ULL * 1024ULL));
  }
  xSemaphoreGive(sdMutex);

  if (written != static_cast<size_t>(rowLength)) {
    Serial.printf("[FLOW] short write: %u/%u bytes\n",
                  static_cast<unsigned int>(written), rowLength);
    return false;
  }
  customerCount = newCount;
  Serial.printf("[FLOW] %s count=%lu\n", timestamp,
                static_cast<unsigned long>(newCount));
  return true;
}

uint8_t rgb565ToGray(uint16_t pixel) {
  const uint16_t red = ((pixel >> 11) & 0x1f) * 255 / 31;
  const uint16_t green = ((pixel >> 5) & 0x3f) * 255 / 63;
  const uint16_t blue = (pixel & 0x1f) * 255 / 31;
  return static_cast<uint8_t>((red * 77 + green * 150 + blue * 29) >> 8);
}

bool analyzeMotion(const uint8_t *jpeg, size_t jpegLength, bool *detected,
                   uint16_t *score, uint16_t *frameScore,
                   uint16_t *motionCenterX, uint8_t *averageBrightness,
                   uint16_t *brightRatio, uint16_t *darkRatio) {
  *detected = false;
  *score = 0;
  *frameScore = 0;
  *motionCenterX = kInvalidMotionCenterX;
  *averageBrightness = 0;
  *brightRatio = 0;
  *darkRatio = 0;
  if (!jpg2rgb565(jpeg, jpegLength, motionRgb565, JPG_SCALE_8X)) {
    return false;
  }

  size_t roiIndex = 0;
  int32_t brightnessDeltaSum = 0;
  uint32_t brightnessSum = 0;
  size_t brightPixels = 0;
  size_t darkPixels = 0;
  for (uint16_t y = kRoiTop; y < kRoiTop + kRoiHeight; ++y) {
    for (uint16_t x = kRoiLeft; x < kRoiLeft + kRoiWidth; ++x) {
      const size_t pixelIndex = (y * kMotionWidth + x) * 2;
      const uint16_t pixel = motionRgb565[pixelIndex] |
                             (motionRgb565[pixelIndex + 1] << 8);
      currentRoi[roiIndex] = rgb565ToGray(pixel);
      brightnessSum += currentRoi[roiIndex];
      if (currentRoi[roiIndex] >= kExposureBrightPixelThreshold) {
        ++brightPixels;
      }
      if (currentRoi[roiIndex] <= kExposureDarkPixelThreshold) {
        ++darkPixels;
      }
      if (motionBaselineReady) {
        brightnessDeltaSum += static_cast<int32_t>(currentRoi[roiIndex]) -
                              static_cast<int32_t>(backgroundRoi[roiIndex]);
      }
      ++roiIndex;
    }
  }

  *averageBrightness = static_cast<uint8_t>(brightnessSum / kRoiPixels);
  *brightRatio =
      static_cast<uint16_t>((brightPixels * 10000UL) / kRoiPixels);
  *darkRatio = static_cast<uint16_t>((darkPixels * 10000UL) / kRoiPixels);

  if (!motionBaselineReady) {
    std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
    std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
    motionBaselineReady = true;
    return true;
  }

  const int32_t averageBrightnessDelta = brightnessDeltaSum / kRoiPixels;
  size_t changedPixels = 0;
  size_t changedXSum = 0;
  for (size_t i = 0; i < kRoiPixels; ++i) {
    const int32_t pixelDelta = static_cast<int32_t>(currentRoi[i]) -
                               static_cast<int32_t>(backgroundRoi[i]) -
                               averageBrightnessDelta;
    if (std::abs(pixelDelta) >= kPixelDeltaThreshold) {
      ++changedPixels;
      changedXSum += i % kRoiWidth;
    }
  }
  *score = static_cast<uint16_t>((changedPixels * 10000UL) / kRoiPixels);
  if (changedPixels != 0) {
    *motionCenterX = static_cast<uint16_t>(changedXSum / changedPixels);
  }
  *detected = *score >= kMotionRatioThreshold &&
              *score <= kGlobalChangeThreshold;

  int32_t frameBrightnessDeltaSum = 0;
  for (size_t i = 0; i < kRoiPixels; ++i) {
    frameBrightnessDeltaSum += static_cast<int32_t>(currentRoi[i]) -
                               static_cast<int32_t>(previousRoi[i]);
  }
  const int32_t frameBrightnessDelta =
      frameBrightnessDeltaSum / kRoiPixels;
  size_t frameChangedPixels = 0;
  for (size_t i = 0; i < kRoiPixels; ++i) {
    const int32_t pixelDelta = static_cast<int32_t>(currentRoi[i]) -
                               static_cast<int32_t>(previousRoi[i]) -
                               frameBrightnessDelta;
    if (std::abs(pixelDelta) >= kPixelDeltaThreshold) {
      ++frameChangedPixels;
    }
  }
  *frameScore = static_cast<uint16_t>(
      (frameChangedPixels * 10000UL) / kRoiPixels);
  if (*score <= kGlobalChangeThreshold) {
    std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
  }

  if (*score < kMotionRatioThreshold) {
    for (size_t i = 0; i < kRoiPixels; ++i) {
      backgroundRoi[i] = static_cast<uint8_t>(
          (static_cast<uint16_t>(backgroundRoi[i]) * 7 + currentRoi[i]) / 8);
    }
  }
  return true;
}

void motionTask(void *) {
  bool armedMotionActive = false;
  bool pendingFlowEvent = false;
  bool detectorCalibrated = false;
  bool approachCandidate = false;
  bool farTargetSeen = false;
  bool nearTargetSeen = false;
  uint8_t runtimeGlobalFrames = 0;
  uint8_t stationaryMotionFrames = 0;
  uint8_t stableFrames = 0;
  uint8_t approachMissingFrames = 0;
  uint8_t approachActiveFrames = 0;
  uint8_t partialExitFrames = 0;
  uint16_t approachStartScore = 0;
  uint16_t approachMinScore = 0;
  uint16_t approachMaxScore = 0;
  uint16_t exitReferenceScore = 0;
  uint32_t approachStartedAt = 0;
  uint32_t lastCountAt = 0;
  uint8_t diagnosticDivider = 0;
  uint8_t warmupSamples = kMotionWarmupSamples;
  uint8_t abnormalExposureFrames = 0;
  uint8_t autoExposureSettleFrames = 0;
  uint32_t lastExposureRecoveryAt = 0;
  bool autoControlsLocked = false;
  bool sourceLogged = false;
  bool lastFrameFromStream = false;
  motionDetectorArmed = false;
  approachNearConfirmed = false;
  motionTaskReady = true;

  auto resetApproachTracking = [&]() {
    approachCandidate = false;
    approachTrackingActive = false;
    farTargetSeen = false;
    nearTargetSeen = false;
    approachNearConfirmed = false;
    approachMissingFrames = 0;
    approachActiveFrames = 0;
    partialExitFrames = 0;
    approachStartScore = 0;
    approachMinScore = 0;
    approachMaxScore = 0;
    exitReferenceScore = 0;
    approachStartedAt = 0;
  };

  while (true) {
    uint8_t *jpeg = nullptr;
    size_t jpegLength = 0;
    bool frameFromStream = false;
    MotionFrame queuedFrame {};
    if (xQueueReceive(motionFrameQueue, &queuedFrame, 0) == pdTRUE) {
      jpeg = queuedFrame.jpeg;
      jpegLength = queuedFrame.length;
      frameFromStream = true;
    } else if (streamClientActive) {
      const uint32_t streamWaitMs =
          approachTrackingActive ? kApproachSampleIntervalMs + 100
                                 : kMotionSampleIntervalMs + 100;
      if (xQueueReceive(motionFrameQueue, &queuedFrame,
                        pdMS_TO_TICKS(streamWaitMs)) == pdTRUE) {
        jpeg = queuedFrame.jpeg;
        jpegLength = queuedFrame.length;
        frameFromStream = true;
      } else {
        Serial.println("[MOTION] stream frame delayed; keeping source consistent");
        continue;
      }
    } else {
      const uint32_t sampleInterval =
          approachTrackingActive
              ? kCameraApproachSampleIntervalMs
              : (detectorCalibrated ? kCameraArmedMotionSampleIntervalMs
                                    : kCameraMotionSampleIntervalMs);
      vTaskDelay(pdMS_TO_TICKS(sampleInterval));
      if (!captureJpeg(FRAMESIZE_VGA, &jpeg, &jpegLength)) {
        Serial.println("[MOTION] frame unavailable");
        continue;
      }
    }
    const bool sourceChanged = sourceLogged &&
                               frameFromStream != lastFrameFromStream;
    if (!sourceLogged || sourceChanged) {
      Serial.printf("[MOTION] source: %s\n",
                    frameFromStream ? "reused MJPEG frame" : "camera fallback");
      if (sourceChanged) {
        resetApproachTracking();
        motionBaselineReady = false;
        detectorCalibrated = false;
        motionDetectorArmed = false;
        motionDetected = false;
        warmupSamples = 2;
        Serial.println("[MOTION] source changed; rebuilding background");
      }
      sourceLogged = true;
      lastFrameFromStream = frameFromStream;
    }

    bool detected = false;
    uint16_t score = 0;
    uint16_t frameScore = 0;
    uint16_t motionCenterX = kInvalidMotionCenterX;
    uint8_t averageBrightness = 0;
    uint16_t brightRatio = 0;
    uint16_t darkRatio = 0;
    const bool analyzed =
        analyzeMotion(jpeg, jpegLength, &detected, &score, &frameScore,
                      &motionCenterX, &averageBrightness, &brightRatio,
                      &darkRatio);
    free(jpeg);
    if (!analyzed) {
      Serial.println("[MOTION] JPEG decode failed");
      continue;
    }

    motionScore = score;
    cameraBrightness = averageBrightness;
    cameraBrightRatio = brightRatio;
    cameraDarkRatio = darkRatio;

    if (exposureAdjusting) {
      motionDetected = false;
      std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
      std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
      if (autoExposureSettleFrames != 0) {
        --autoExposureSettleFrames;
      }
      if (autoExposureSettleFrames == 0) {
        if (lockCameraAutoControls()) {
          exposureAdjusting = false;
          autoControlsLocked = true;
          lastExposureRecoveryAt = millis();
          abnormalExposureFrames = 0;
          warmupSamples = 2;
          Serial.printf(
              "[CAM] exposure stabilized and relocked: mean=%u bright=%u.%02u%% dark=%u.%02u%%\n",
              averageBrightness, brightRatio / 100, brightRatio % 100,
              darkRatio / 100, darkRatio % 100);
        } else {
          autoExposureSettleFrames = 2;
        }
      }
      continue;
    }

    if (warmupSamples != 0) {
      motionDetected = false;
      std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
      std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
      --warmupSamples;
      if (warmupSamples == 0) {
        if (!autoControlsLocked) {
          if (lockCameraAutoControls()) {
            autoControlsLocked = true;
            warmupSamples = 2;
            Serial.println(
                "[MOTION] camera controls locked; rebuilding background");
          } else {
            warmupSamples = 2;
            Serial.println("[MOTION] camera control lock failed; retrying");
          }
        } else {
          detectorCalibrated = true;
          motionDetectorArmed = true;
          Serial.println("[MOTION] short calibration complete; detector armed");
        }
      }
      continue;
    }

    const uint32_t exposureNow = millis();
    const bool exposureCooldownComplete =
        lastExposureRecoveryAt == 0 ||
        exposureNow - lastExposureRecoveryAt >= kExposureRecoveryCooldownMs;
    const bool stableSceneForExposure =
        frameScore < kSceneStableRatioThreshold && !approachCandidate &&
        !armedMotionActive;
    const bool severelyOverexposed =
        averageBrightness >= kExposureOverexposedMeanThreshold &&
        brightRatio >= kExposureOverexposedRatioThreshold;
    const bool severelyUnderexposed =
        averageBrightness <= kExposureUnderexposedMeanThreshold &&
        darkRatio >= kExposureUnderexposedRatioThreshold;
    if (autoControlsLocked && exposureCooldownComplete &&
        stableSceneForExposure &&
        (severelyOverexposed || severelyUnderexposed)) {
      if (abnormalExposureFrames < kExposureAbnormalFramesToRecover) {
        ++abnormalExposureFrames;
      }
    } else {
      abnormalExposureFrames = 0;
    }
    if (abnormalExposureFrames >= kExposureAbnormalFramesToRecover) {
      resetApproachTracking();
      detectorCalibrated = false;
      motionDetectorArmed = false;
      motionDetected = false;
      if (enableCameraAutoControls()) {
        autoControlsLocked = false;
        exposureAdjusting = true;
        autoExposureSettleFrames = kExposureAutoSettleFrames;
        motionBaselineReady = false;
        Serial.printf(
            "[CAM] sustained %s detected: mean=%u bright=%u.%02u%% dark=%u.%02u%%; recalibrating exposure\n",
            severelyOverexposed ? "overexposure" : "underexposure",
            averageBrightness, brightRatio / 100, brightRatio % 100,
            darkRatio / 100, darkRatio % 100);
      } else {
        abnormalExposureFrames = 0;
        lastExposureRecoveryAt = exposureNow;
      }
      continue;
    }

    if (pendingFlowEvent) {
      if (appendFlowEvent()) {
        pendingFlowEvent = false;
        lastCountAt = millis();
        Serial.println("[FLOW] queued event persisted");
      } else {
        Serial.println("[FLOW] event still pending; will retry");
        continue;
      }
    }

    const bool globalChange = score > kGlobalChangeThreshold;
    if (globalChange) {
      motionDetected = false;
      if (approachCandidate) {
        Serial.println("[APPROACH] tracking cancelled: global scene change");
        resetApproachTracking();
      }
      if (++runtimeGlobalFrames >= 3) {
        std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
        std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
        runtimeGlobalFrames = 0;
        Serial.println("[MOTION] background recovered after global scene change");
      }
      continue;
    }
    runtimeGlobalFrames = 0;

    if (detected && !approachCandidate &&
        frameScore < kSceneStableRatioThreshold) {
      if (stationaryMotionFrames < kStableMotionFramesToRebase) {
        ++stationaryMotionFrames;
      }
    } else {
      stationaryMotionFrames = 0;
    }
    if (stationaryMotionFrames >= kStableMotionFramesToRebase) {
      std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
      std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
      stationaryMotionFrames = 0;
      motionScore = 0;
      motionDetected = false;
      if (approachCandidate) {
        resetApproachTracking();
      }
      Serial.println("[MOTION] stationary change absorbed into background");
      continue;
    }

    motionDetected = detected;
    if (armedMotionActive) {
      const uint32_t now = millis();
      if (frameScore < kSceneStableRatioThreshold) {
        if (stableFrames < kStableFramesToRearm) {
          ++stableFrames;
        }
      } else {
        stableFrames = 0;
      }
      if (stableFrames >= kStableFramesToRearm &&
          now - lastCountAt >= kMotionCooldownMs) {
        std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
        armedMotionActive = false;
        stableFrames = 0;
        motionDetected = false;
        Serial.println("[MOTION] stable scene captured; detector rearmed");
      }
      continue;
    }

    const uint32_t now = millis();
    if (approachCandidate &&
        now - approachStartedAt >= kApproachTrackingTimeoutMs) {
      Serial.printf(
          "[APPROACH] tracking timed out: far=%s near=%s min=%u.%02u%% max=%u.%02u%%\n",
          farTargetSeen ? "yes" : "no", nearTargetSeen ? "yes" : "no",
          approachMinScore / 100, approachMinScore % 100,
          approachMaxScore / 100, approachMaxScore % 100);
      std::memcpy(backgroundRoi, currentRoi, sizeof(backgroundRoi));
      std::memcpy(previousRoi, currentRoi, sizeof(previousRoi));
      motionScore = 0;
      motionDetected = false;
      resetApproachTracking();
      continue;
    }

    if (detected && motionCenterX != kInvalidMotionCenterX) {
      stableFrames = 0;
      if (!approachCandidate) {
        if (frameScore >= kSceneStableRatioThreshold) {
          approachCandidate = true;
          approachTrackingActive = true;
          approachStartScore = score;
          approachMinScore = score;
          approachMaxScore = score;
          approachActiveFrames = 1;
          farTargetSeen = score <= kApproachFarScoreMax;
          nearTargetSeen = false;
          approachStartedAt = now;
          approachMissingFrames = 0;
          Serial.printf(
              "[APPROACH] moving target acquired, size=%u.%02u%% far=%s\n",
              score / 100, score % 100, farTargetSeen ? "yes" : "no");
        }
      } else {
        approachMissingFrames = 0;
        if (approachActiveFrames < UINT8_MAX) {
          ++approachActiveFrames;
        }
        approachMinScore = std::min(approachMinScore, score);
        approachMaxScore = std::max(approachMaxScore, score);
        if (score <= kApproachFarScoreMax) {
          farTargetSeen = true;
        }
        const bool grewEnough =
            approachMaxScore >= approachMinScore + kApproachRequiredGrowth;
        if (!nearTargetSeen && farTargetSeen && grewEnough &&
            approachActiveFrames >= kApproachMinActiveFrames &&
            approachMaxScore >= kApproachNearScoreMin) {
          nearTargetSeen = true;
          approachNearConfirmed = true;
          exitReferenceScore = approachMaxScore;
          Serial.printf(
              "[APPROACH] near target confirmed, size=%u.%02u%%->%u.%02u%%; waiting for disappearance\n",
              approachMinScore / 100, approachMinScore % 100,
              approachMaxScore / 100, approachMaxScore % 100);
        }
        if (nearTargetSeen) {
          if (score > exitReferenceScore) {
            exitReferenceScore = score;
            partialExitFrames = 0;
          } else {
            const bool absoluteDrop =
                exitReferenceScore >= score + kApproachPartialExitDrop;
            const bool relativeDrop =
                static_cast<uint32_t>(score) * 100UL <=
                static_cast<uint32_t>(exitReferenceScore) *
                    kApproachRemainingPercent;
            const bool anotherTargetRemains =
                score >= kApproachRemainingTargetMin;
            if (absoluteDrop && relativeDrop && anotherTargetRemains) {
              if (partialExitFrames < kApproachPartialExitFrames) {
                ++partialExitFrames;
              }
              if (partialExitFrames >= kApproachPartialExitFrames) {
                const bool cooldownComplete =
                    lastCountAt == 0 ||
                    now - lastCountAt >= kApproachExitCooldownMs;
                if (cooldownComplete) {
                  pendingFlowEvent = true;
                  if (appendFlowEvent()) {
                    pendingFlowEvent = false;
                    lastCountAt = now;
                  } else {
                    Serial.println("[FLOW] partial-exit event queued for retry");
                  }
                  Serial.printf(
                      "[APPROACH] partial disappearance counted, coverage=%u.%02u%%->%u.%02u%%\n",
                      exitReferenceScore / 100, exitReferenceScore % 100,
                      score / 100, score % 100);
                }
                exitReferenceScore = score;
                partialExitFrames = 0;
              }
            } else {
              partialExitFrames = 0;
            }
          }
        }
      }
    } else if (approachCandidate) {
      if (++approachMissingFrames >= kApproachMissingFramesToConfirmExit) {
        if (nearTargetSeen) {
          const bool cooldownComplete =
              lastCountAt == 0 ||
              now - lastCountAt >= kApproachExitCooldownMs;
          if (cooldownComplete) {
            pendingFlowEvent = true;
            if (appendFlowEvent()) {
              pendingFlowEvent = false;
              lastCountAt = now;
            } else {
              Serial.println("[FLOW] event queued for retry");
            }
            Serial.printf(
                "[APPROACH] near-side disappearance counted, size=%u.%02u%%->%u.%02u%%\n",
                approachStartScore / 100, approachStartScore % 100,
                approachMaxScore / 100, approachMaxScore % 100);
          } else {
            Serial.println(
                "[APPROACH] near-side disappearance ignored during cooldown");
          }
          armedMotionActive = true;
        } else {
          Serial.printf(
              "[APPROACH] disappearance ignored: target did not grow from far to near, min=%u.%02u%% max=%u.%02u%%\n",
              approachMinScore / 100, approachMinScore % 100,
              approachMaxScore / 100, approachMaxScore % 100);
        }
        resetApproachTracking();
      }
    }

    if (++diagnosticDivider >= 7) {
      diagnosticDivider = 0;
      Serial.printf("[MOTION] score=%u.%02u%% state=%s count=%lu\n", score / 100,
                    score % 100, detected ? "ACTIVE" : "idle",
                    static_cast<unsigned long>(customerCount));
    }
  }
}

void snapshotTask(void *) {
  TickType_t lastWake = xTaskGetTickCount();
  while (true) {
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(kCaptureIntervalMs));
    if (!sdReady) {
      continue;
    }

    const uint32_t startedAt = millis();
    uint8_t *jpeg = nullptr;
    size_t jpegLength = 0;
    if (!captureJpeg(FRAMESIZE_VGA, &jpeg, &jpegLength)) {
      ++captureFailures;
      Serial.println("[SHOT] capture or JPEG conversion failed");
      continue;
    }

    const uint32_t imageIndex = nextImageIndex;
    const bool saved = saveJpeg(jpeg, jpegLength, imageIndex);
    free(jpeg);

    if (saved) {
      nextImageIndex = imageIndex + 1;
      ++savedThisBoot;
      lastImageBytes = static_cast<uint32_t>(jpegLength);
      lastWriteMs = millis() - startedAt;
    } else {
      ++captureFailures;
    }
  }
}

esp_err_t indexHandler(httpd_req_t *request) {
  if (WiFi.status() != WL_CONNECTED) {
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "/network");
    return httpd_resp_sendstr(request, "正在打开 Wi-Fi 配置页面...");
  }
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

int hexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool urlDecode(const char *source, size_t length, char *target,
               size_t targetSize) {
  if (targetSize == 0) return false;
  size_t output = 0;
  for (size_t i = 0; i < length; ++i) {
    char value = source[i];
    if (value == '+') {
      value = ' ';
    } else if (value == '%' && i + 2 < length) {
      const int high = hexDigit(source[i + 1]);
      const int low = hexDigit(source[i + 2]);
      if (high < 0 || low < 0) return false;
      value = static_cast<char>((high << 4) | low);
      i += 2;
    }
    if (output + 1 >= targetSize) return false;
    target[output++] = value;
  }
  target[output] = '\0';
  return true;
}

bool formValue(const char *body, const char *key, char *value,
               size_t valueSize) {
  const size_t keyLength = std::strlen(key);
  const char *cursor = body;
  while (cursor != nullptr && *cursor != '\0') {
    const char *separator = std::strchr(cursor, '&');
    const size_t fieldLength = separator == nullptr
                                   ? std::strlen(cursor)
                                   : static_cast<size_t>(separator - cursor);
    if (fieldLength > keyLength &&
        std::strncmp(cursor, key, keyLength) == 0 &&
        cursor[keyLength] == '=') {
      return urlDecode(cursor + keyLength + 1, fieldLength - keyLength - 1,
                       value, valueSize);
    }
    cursor = separator == nullptr ? nullptr : separator + 1;
  }
  return false;
}

bool receiveBody(httpd_req_t *request, char *body, size_t bodySize) {
  const size_t length = request->content_len;
  if (length == 0 || length >= bodySize) return false;
  size_t received = 0;
  while (received < length) {
    const int result = httpd_req_recv(request, body + received,
                                      length - received);
    if (result <= 0) return false;
    received += static_cast<size_t>(result);
  }
  body[received] = '\0';
  return true;
}

esp_err_t networkPageHandler(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, kNetworkHtml, HTTPD_RESP_USE_STRLEN);
}

void appendJsonString(String &json, const String &value) {
  json += '"';
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character == '\\' || character == '"') {
      json += '\\';
      json += character;
    } else if (static_cast<uint8_t>(character) >= 0x20) {
      json += character;
    }
  }
  json += '"';
}

esp_err_t networkScanHandler(httpd_req_t *request) {
  const int networkCount = WiFi.scanNetworks(false, true, false, 250);
  if (networkCount < 0) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Wi-Fi scan failed");
  }

  String json;
  json.reserve(2048);
  json = "{\"networks\":[";
  bool first = true;
  for (int index = 0; index < networkCount; ++index) {
    const String ssid = WiFi.SSID(index);
    if (ssid.isEmpty()) continue;

    bool duplicate = false;
    for (int previous = 0; previous < index; ++previous) {
      if (ssid == WiFi.SSID(previous)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    if (!first) json += ',';
    first = false;
    json += "{\"ssid\":";
    appendJsonString(json, ssid);
    json += ",\"rssi\":";
    json += String(WiFi.RSSI(index));
    json += ",\"secure\":";
    json += WiFi.encryptionType(index) == WIFI_AUTH_OPEN ? "false" : "true";
    json += '}';
  }
  json += "]}";
  WiFi.scanDelete();
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, json.c_str(), json.length());
}

esp_err_t captivePortalHandler(httpd_req_t *request) {
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "/network");
  return httpd_resp_sendstr(request, "正在打开 Wi-Fi 配置页面...");
}

esp_err_t networkStatusHandler(httpd_req_t *request) {
  Preferences preferences;
  preferences.begin("wifi", true);
  const String configuredSsid = preferences.getString("ssid", "");
  preferences.end();
  const String connectedSsid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
  const String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  char json[256];
  std::snprintf(json, sizeof(json),
                "{\"connected\":%s,\"configuredSsid\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"hostname\":\"esp32cam.local\"}",
                WiFi.status() == WL_CONNECTED ? "true" : "false",
                configuredSsid.c_str(), connectedSsid.c_str(), ip.c_str());
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t networkSaveHandler(httpd_req_t *request) {
  char body[260];
  char ssid[33];
  char password[64];
  if (!receiveBody(request, body, sizeof(body)) ||
      !formValue(body, "ssid", ssid, sizeof(ssid)) ||
      !formValue(body, "password", password, sizeof(password)) ||
      ssid[0] == '\0') {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "invalid ssid or password");
  }
  Preferences preferences;
  if (!preferences.begin("wifi", false)) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "preferences unavailable");
  }
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  wifiRestartRequested = true;
  wifiRestartRequestedAt = millis();
  Serial.printf("[WIFI] credentials saved for SSID=%s; restarting\n", ssid);
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  return httpd_resp_sendstr(request,
                            "已保存。设备将在约 1 秒后重启，请等待局域网连接。");
}

void connectStoredWiFi() {
  Preferences preferences;
  preferences.begin("wifi", true);
  const String ssid = preferences.getString("ssid", "");
  const String password = preferences.getString("password", "");
  preferences.end();
  if (ssid.isEmpty()) {
    Serial.println("[WIFI] no router credentials; AP fallback only");
    return;
  }

  Serial.printf("[WIFI] connecting to SSID=%s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t deadline = millis() + 12000;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WIFI] router connection failed (status=%d); AP remains available\n",
                  static_cast<int>(WiFi.status()));
    WiFi.disconnect(false, false);
    return;
  }

  Serial.printf("[WIFI] LAN connected: SSID=%s IP=%s RSSI=%d dBm\n",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  if (MDNS.begin("esp32cam")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[WIFI] mDNS: http://esp32cam.local/");
  } else {
    Serial.println("[WIFI] mDNS start failed; use the printed LAN IP");
  }
}

esp_err_t statusHandler(httpd_req_t *request) {
  char json[448];
  std::snprintf(json, sizeof(json),
                "{\"sd\":%s,\"saved\":%lu,\"failures\":%lu,\"lastBytes\":%lu,\"lastWriteMs\":%lu,\"freeMB\":%lu,\"nextImage\":%lu,\"count\":%lu,\"armed\":%s,\"approach\":%s,\"near\":%s,\"motion\":%s,\"motionScore\":%u.%02u,\"exposureAdjusting\":%s,\"brightness\":%u,\"brightRatio\":%u.%02u,\"darkRatio\":%u.%02u}",
                sdReady ? "true" : "false",
                static_cast<unsigned long>(savedThisBoot),
                static_cast<unsigned long>(captureFailures),
                static_cast<unsigned long>(lastImageBytes),
                static_cast<unsigned long>(lastWriteMs),
                static_cast<unsigned long>(freeSpaceMb),
                static_cast<unsigned long>(nextImageIndex),
                static_cast<unsigned long>(customerCount),
                motionDetectorArmed ? "true" : "false",
                approachTrackingActive ? "true" : "false",
                approachNearConfirmed ? "true" : "false",
                motionDetected ? "true" : "false", motionScore / 100,
                motionScore % 100, exposureAdjusting ? "true" : "false",
                cameraBrightness, cameraBrightRatio / 100,
                cameraBrightRatio % 100, cameraDarkRatio / 100,
                cameraDarkRatio % 100);
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

bool readSdFile(const char *path, uint8_t **data, size_t *length,
                size_t maximumLength) {
  *data = nullptr;
  *length = 0;
  if (!sdReady || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return false;
  }

  File input = SD_MMC.open(path, FILE_READ);
  if (!input || input.isDirectory()) {
    if (input) {
      input.close();
    }
    xSemaphoreGive(sdMutex);
    return false;
  }

  const size_t fileLength = input.size();
  if (fileLength == 0 || fileLength > maximumLength) {
    input.close();
    xSemaphoreGive(sdMutex);
    return false;
  }

  uint8_t *buffer = static_cast<uint8_t *>(malloc(fileLength));
  if (buffer == nullptr) {
    input.close();
    xSemaphoreGive(sdMutex);
    return false;
  }
  const size_t bytesRead = input.read(buffer, fileLength);
  input.close();
  xSemaphoreGive(sdMutex);
  if (bytesRead != fileLength) {
    free(buffer);
    return false;
  }

  *data = buffer;
  *length = fileLength;
  return true;
}

esp_err_t chartJsHandler(httpd_req_t *request) {
  const size_t embeddedLength = chartJsEnd - chartJsStart;
  const size_t length = embeddedLength == 0 ? 0 : embeddedLength - 1;
  httpd_resp_set_type(request, "application/javascript; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(
      request, reinterpret_cast<const char *>(chartJsStart), length);
}

esp_err_t flowCsvHandler(httpd_req_t *request) {
  if (!sdReady || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[HTTP] flow.csv SD lock timeout");
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "flow.csv unavailable");
  }

  File input = SD_MMC.open(kFlowCsvPath, FILE_READ);
  if (!input || input.isDirectory()) {
    if (input) {
      input.close();
    }
    xSemaphoreGive(sdMutex);
    Serial.println("[HTTP] flow.csv open failed");
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "flow.csv unavailable");
  }

  httpd_resp_set_type(request, "text/csv; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  uint8_t buffer[1024];
  size_t totalSent = 0;
  esp_err_t result = ESP_OK;
  while (input.available() && result == ESP_OK) {
    const size_t bytesRead = input.read(buffer, sizeof(buffer));
    if (bytesRead == 0) {
      break;
    }
    result = httpd_resp_send_chunk(
        request, reinterpret_cast<const char *>(buffer), bytesRead);
    totalSent += bytesRead;
  }
  input.close();
  xSemaphoreGive(sdMutex);
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nullptr, 0);
  }
  Serial.printf("[HTTP] flow.csv %s: %u bytes\n",
                result == ESP_OK ? "sent" : "send failed",
                static_cast<unsigned int>(totalSent));
  return result;
}

esp_err_t photoCatalogHandler(httpd_req_t *request) {
  uint32_t afterIndex = 0;
  char query[64];
  char afterText[20];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "after", afterText, sizeof(afterText)) ==
          ESP_OK) {
    afterIndex = static_cast<uint32_t>(std::strtoul(afterText, nullptr, 10));
  }

  if (!sdReady || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "photo catalog unavailable");
  }
  File catalog = SD_MMC.open(kPhotoCatalogPath, FILE_READ);
  if (!catalog || catalog.isDirectory()) {
    if (catalog) catalog.close();
    xSemaphoreGive(sdMutex);
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "photo catalog unavailable");
  }

  const size_t recordCount = catalog.size() / sizeof(PhotoCatalogRecord);
  size_t lower = 0;
  size_t upper = recordCount;
  PhotoCatalogRecord record {};
  while (lower < upper) {
    const size_t middle = lower + (upper - lower) / 2;
    if (!readCatalogRecord(catalog, middle, &record)) {
      catalog.close();
      xSemaphoreGive(sdMutex);
      return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "photo catalog read failed");
    }
    if (record.index <= afterIndex) {
      lower = middle + 1;
    } else {
      upper = middle;
    }
  }

  httpd_resp_set_type(request, "text/csv; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send_chunk(request, "index,epoch\n", 12);
  size_t sentRecords = 0;
  char output[768];
  size_t outputLength = 0;
  if (result == ESP_OK && catalog.seek(lower * sizeof(PhotoCatalogRecord))) {
    while (catalog.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) ==
               sizeof(record) &&
           result == ESP_OK) {
      char line[48];
      const int lineLength = std::snprintf(
          line, sizeof(line), "%lu,%lld\n",
          static_cast<unsigned long>(record.index),
          static_cast<long long>(record.epoch));
      if (lineLength <= 0) continue;
      if (outputLength + static_cast<size_t>(lineLength) > sizeof(output)) {
        result = httpd_resp_send_chunk(request, output, outputLength);
        outputLength = 0;
      }
      if (result == ESP_OK) {
        std::memcpy(output + outputLength, line, lineLength);
        outputLength += static_cast<size_t>(lineLength);
        ++sentRecords;
      }
    }
  }
  if (result == ESP_OK && outputLength != 0) {
    result = httpd_resp_send_chunk(request, output, outputLength);
  }
  catalog.close();
  xSemaphoreGive(sdMutex);
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nullptr, 0);
  }
  Serial.printf("[HTTP] photo catalog after=%lu records=%u status=%s\n",
                static_cast<unsigned long>(afterIndex),
                static_cast<unsigned int>(sentRecords),
                result == ESP_OK ? "sent" : "failed");
  return result;
}

esp_err_t timelapseHandler(httpd_req_t *request) {
  const uint32_t oldest = oldestImageIndex;
  const uint32_t next = nextImageIndex;
  const uint32_t count = next > oldest ? next - oldest : 0;
  char json[128];
  std::snprintf(json, sizeof(json),
                "{\"oldest\":%lu,\"next\":%lu,\"count\":%lu,\"fps\":15}",
                static_cast<unsigned long>(oldest),
                static_cast<unsigned long>(next),
                static_cast<unsigned long>(count));
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t photoHandler(httpd_req_t *request) {
  char query[64];
  char indexText[20];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "index", indexText, sizeof(indexText)) != ESP_OK) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "missing index");
  }

  char *end = nullptr;
  const uint32_t index = static_cast<uint32_t>(std::strtoul(indexText, &end, 10));
  if (end == indexText || *end != '\0' || index < oldestImageIndex ||
      index >= nextImageIndex) {
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "photo not found");
  }

  char path[64];
  uint8_t *data = nullptr;
  size_t length = 0;
  if (!resolveImagePath(index, path, sizeof(path)) ||
      !readSdFile(path, &data, &length, 512 * 1024)) {
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "photo not found");
  }
  httpd_resp_set_type(request, "image/jpeg");
  httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
  const esp_err_t result = httpd_resp_send(
      request, reinterpret_cast<const char *>(data), length);
  free(data);
  return result;
}

esp_err_t timeHandler(httpd_req_t *request) {
  char query[64];
  char epochText[24];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "epoch", epochText, sizeof(epochText)) != ESP_OK) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "missing epoch");
  }

  const time_t epoch = static_cast<time_t>(std::strtoull(epochText, nullptr, 10));
  if (epoch < 1700000000) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid epoch");
  }
  const struct timeval now = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&now, nullptr);
  Serial.printf("[TIME] synchronized: %llu\n",
                static_cast<unsigned long long>(epoch));
  maintainDailyCustomerCount();
  return httpd_resp_sendstr(request, "OK");
}

esp_err_t streamHandler(httpd_req_t *request) {
  static constexpr char kContentType[] = "multipart/x-mixed-replace;boundary=frame";
  static constexpr char kBoundary[] = "\r\n--frame\r\n";
  static constexpr char kPartHeader[] = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

  esp_err_t result = httpd_resp_set_type(request, kContentType);
  if (result != ESP_OK) {
    return result;
  }
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  const int socket = httpd_req_to_sockfd(request);
  const int noDelay = 1;
  if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                 sizeof(noDelay)) != 0) {
    Serial.println("[STREAM] TCP_NODELAY unavailable");
  }

  streamClientActive = true;
  uint32_t lastMotionSampleAt = 0;
  uint32_t streamStatsAt = millis();
  uint32_t streamFrames = 0;
  while (true) {
    const uint32_t frameStartedAt = millis();
    uint8_t *jpeg = nullptr;
    size_t jpegLength = 0;
    if (!captureJpeg(FRAMESIZE_VGA, &jpeg, &jpegLength)) {
      Serial.println("[STREAM] capture or JPEG conversion failed");
      result = ESP_FAIL;
      break;
    }

    char header[64];
    const size_t headerLength = std::snprintf(
        header, sizeof(header), kPartHeader, static_cast<unsigned int>(jpegLength));

    result = httpd_resp_send_chunk(request, kBoundary, sizeof(kBoundary) - 1);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(request, header, headerLength);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(jpeg), jpegLength);
    }
    bool queuedForMotion = false;
    const uint32_t motionNow = millis();
    const uint32_t motionInterval = approachTrackingActive
                                        ? kApproachSampleIntervalMs
                                        : kMotionSampleIntervalMs;
    if (result == ESP_OK && motionTaskReady &&
        motionNow - lastMotionSampleAt >= motionInterval) {
      const MotionFrame motionFrame = {.jpeg = jpeg, .length = jpegLength};
      queuedForMotion =
          xQueueSend(motionFrameQueue, &motionFrame, 0) == pdTRUE;
      if (queuedForMotion) {
        lastMotionSampleAt = motionNow;
      }
    }
    if (!queuedForMotion) {
      free(jpeg);
    }

    if (result != ESP_OK) {
      break;
    }
    ++streamFrames;
    const uint32_t statsNow = millis();
    const uint32_t statsElapsed = statsNow - streamStatsAt;
    if (statsElapsed >= 10000) {
      const uint32_t fpsTimes100 =
          static_cast<uint32_t>((streamFrames * 100000ULL) / statsElapsed);
      Serial.printf("[STREAM] actual fps=%lu.%02lu\n",
                    static_cast<unsigned long>(fpsTimes100 / 100),
                    static_cast<unsigned long>(fpsTimes100 % 100));
      streamStatsAt = statsNow;
      streamFrames = 0;
    }
    const uint32_t frameElapsedMs = millis() - frameStartedAt;
    if (frameElapsedMs < kStreamFrameIntervalMs) {
      vTaskDelay(pdMS_TO_TICKS(kStreamFrameIntervalMs - frameElapsedMs));
    }
  }
  streamClientActive = false;
  return result;
}

bool startWebServers() {
  httpd_config_t webConfig = HTTPD_DEFAULT_CONFIG();
  webConfig.server_port = 80;
  webConfig.core_id = 0;
  webConfig.max_uri_handlers = 12;

  if (httpd_start(&webServer, &webConfig) != ESP_OK) {
    Serial.println("[HTTP] web server start failed");
    return false;
  }

  const httpd_uri_t indexUri = {
      .uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = nullptr};
  const httpd_uri_t statusUri = {.uri = "/api/status",
                                 .method = HTTP_GET,
                                 .handler = statusHandler,
                                 .user_ctx = nullptr};
  const httpd_uri_t timeUri = {.uri = "/api/time",
                               .method = HTTP_POST,
                               .handler = timeHandler,
                               .user_ctx = nullptr};
  const httpd_uri_t chartJsUri = {.uri = "/chart.js",
                                  .method = HTTP_GET,
                                  .handler = chartJsHandler,
                                  .user_ctx = nullptr};
  const httpd_uri_t flowCsvUri = {.uri = "/api/flow.csv",
                                  .method = HTTP_GET,
                                  .handler = flowCsvHandler,
                                  .user_ctx = nullptr};
  const httpd_uri_t timelapseUri = {.uri = "/api/timelapse",
                                    .method = HTTP_GET,
                                    .handler = timelapseHandler,
                                    .user_ctx = nullptr};
  const httpd_uri_t photoUri = {.uri = "/api/photo",
                                .method = HTTP_GET,
                                .handler = photoHandler,
                                .user_ctx = nullptr};
  const httpd_uri_t photoCatalogUri = {.uri = "/api/catalog",
                                       .method = HTTP_GET,
                                       .handler = photoCatalogHandler,
                                       .user_ctx = nullptr};
  const httpd_uri_t networkPageUri = {.uri = "/network",
                                      .method = HTTP_GET,
                                      .handler = networkPageHandler,
                                      .user_ctx = nullptr};
  const httpd_uri_t networkStatusUri = {.uri = "/api/network",
                                        .method = HTTP_GET,
                                        .handler = networkStatusHandler,
                                        .user_ctx = nullptr};
  const httpd_uri_t networkScanUri = {.uri = "/api/network/scan",
                                      .method = HTTP_GET,
                                      .handler = networkScanHandler,
                                      .user_ctx = nullptr};
  const httpd_uri_t networkSaveUri = {.uri = "/api/network",
                                      .method = HTTP_POST,
                                      .handler = networkSaveHandler,
                                      .user_ctx = nullptr};
  const httpd_uri_t generate204Uri = {.uri = "/generate_204",
                                      .method = HTTP_GET,
                                      .handler = captivePortalHandler,
                                      .user_ctx = nullptr};
  const httpd_uri_t gen204Uri = {.uri = "/gen_204",
                                 .method = HTTP_GET,
                                 .handler = captivePortalHandler,
                                 .user_ctx = nullptr};
  const httpd_uri_t hotspotDetectUri = {.uri = "/hotspot-detect.html",
                                        .method = HTTP_GET,
                                        .handler = captivePortalHandler,
                                        .user_ctx = nullptr};
  const httpd_uri_t connectTestUri = {.uri = "/connecttest.txt",
                                      .method = HTTP_GET,
                                      .handler = captivePortalHandler,
                                      .user_ctx = nullptr};
  const httpd_uri_t ncsiUri = {.uri = "/ncsi.txt",
                               .method = HTTP_GET,
                               .handler = captivePortalHandler,
                               .user_ctx = nullptr};
  const httpd_uri_t successUri = {.uri = "/success.txt",
                                  .method = HTTP_GET,
                                  .handler = captivePortalHandler,
                                  .user_ctx = nullptr};
  httpd_register_uri_handler(webServer, &indexUri);
  httpd_register_uri_handler(webServer, &statusUri);
  httpd_register_uri_handler(webServer, &timeUri);
  httpd_register_uri_handler(webServer, &chartJsUri);
  httpd_register_uri_handler(webServer, &flowCsvUri);
  httpd_register_uri_handler(webServer, &timelapseUri);
  httpd_register_uri_handler(webServer, &photoUri);
  httpd_register_uri_handler(webServer, &photoCatalogUri);
  httpd_register_uri_handler(webServer, &networkPageUri);
  httpd_register_uri_handler(webServer, &networkStatusUri);
  httpd_register_uri_handler(webServer, &networkScanUri);
  httpd_register_uri_handler(webServer, &networkSaveUri);
  httpd_register_uri_handler(webServer, &generate204Uri);
  httpd_register_uri_handler(webServer, &gen204Uri);
  httpd_register_uri_handler(webServer, &hotspotDetectUri);
  httpd_register_uri_handler(webServer, &connectTestUri);
  httpd_register_uri_handler(webServer, &ncsiUri);
  httpd_register_uri_handler(webServer, &successUri);

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port = 81;
  streamConfig.ctrl_port += 1;
  streamConfig.core_id = 0;
  streamConfig.max_uri_handlers = 2;
  if (httpd_start(&streamServer, &streamConfig) != ESP_OK) {
    Serial.println("[HTTP] stream server start failed");
    return false;
  }

  const httpd_uri_t streamUri = {.uri = "/stream",
                                 .method = HTTP_GET,
                                 .handler = streamHandler,
                                 .user_ctx = nullptr};
  httpd_register_uri_handler(streamServer, &streamUri);
  Serial.println("[HTTP] dashboard: http://192.168.4.1/");
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(500);
  Serial.println("\n[BOOT] ESP32-CAM customer-flow and timelapse gateway");

  setenv("TZ", "CST-8", 1);
  tzset();

  cameraMutex = xSemaphoreCreateMutex();
  sdMutex = xSemaphoreCreateMutex();
  motionFrameQueue = xQueueCreate(1, sizeof(MotionFrame));
  if (cameraMutex == nullptr || sdMutex == nullptr || motionFrameQueue == nullptr) {
    Serial.println("[BOOT] stopped: synchronization primitive unavailable");
    return;
  }

  if (!initializeCamera()) {
    Serial.println("[BOOT] stopped: camera unavailable");
    return;
  }

  uint8_t *testJpeg = nullptr;
  size_t testJpegLength = 0;
  if (!captureJpeg(FRAMESIZE_VGA, &testJpeg, &testJpegLength)) {
    Serial.println("[BOOT] stopped: camera frame/JPEG self-test failed");
    return;
  }
  Serial.printf("[CAM] self-test JPEG: %u bytes\n",
                static_cast<unsigned int>(testJpegLength));
  free(testJpeg);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!WiFi.softAP(kAccessPointName, kAccessPointPassword, 6, false, 2)) {
    Serial.println("[WIFI] access point start failed");
    return;
  }
  Serial.printf("[WIFI] SSID=%s IP=%s\n", kAccessPointName,
                WiFi.softAPIP().toString().c_str());
  captiveDnsStarted = captiveDns.start(53, "*", WiFi.softAPIP());
  Serial.printf("[WIFI] captive portal DNS: %s\n",
                captiveDnsStarted ? "started" : "failed");

  if (!startWebServers()) {
    return;
  }

  connectStoredWiFi();

  initializeSdCard();

  if (sdReady) {
    xTaskCreatePinnedToCore(motionTask, "motion", 8192, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(snapshotTask, "snapshot", 6144, nullptr, 1,
                            nullptr, 1);
  }
}

void loop() {
  if (captiveDnsStarted) captiveDns.processNextRequest();
  if (wifiRestartRequested && millis() - wifiRestartRequestedAt >= 1000) {
    Serial.println("[WIFI] restarting to apply router credentials");
    Serial.flush();
    ESP.restart();
  }
  maintainDailyCustomerCount();
  delay(1000);
}
