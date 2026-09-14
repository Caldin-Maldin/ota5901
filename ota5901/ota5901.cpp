#include "ota5901.h"

#include <Arduino.h>

#ifdef USE_ESP8266
#include <core_esp8266_waveform.h>
extern "C" {
#include <eagle_soc.h>
#include <gpio.h>
}
#endif

#ifdef USE_ESP32
#include <driver/ledc.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_struct.h>
#endif

#include <cinttypes>
#include <cstring>

#include "base64.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ota5901 {

static const char *const TAG = "ota5901";

OTA5901Display *OTA5901Display::te_isr_instance_ = nullptr;

namespace {

inline IRAM_ATTR void gpio_set_mask(uint32_t mask) {
#ifdef USE_ESP8266
  GPIO_REG_WRITE(GPIO_OUT_W1TS_ADDRESS, mask);
#elif defined(USE_ESP32)
  REG_WRITE(GPIO_OUT_W1TS_REG, mask);
#endif
}

inline IRAM_ATTR void gpio_clr_mask(uint32_t mask) {
#ifdef USE_ESP8266
  GPIO_REG_WRITE(GPIO_OUT_W1TC_ADDRESS, mask);
#elif defined(USE_ESP32)
  REG_WRITE(GPIO_OUT_W1TC_REG, mask);
#endif
}

}  // namespace

// ============================================================================
// Ѕраузерный загрузчик /ota5901 (не об€зателен, но полезен дл€ отладки)
// ============================================================================
static const char OTA5901_IMAGE_PAGE[] PROGMEM = R"OTAPAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OTA5901 image uploader v13</title>
<style>
:root{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color-scheme:light dark}body{max-width:860px;margin:24px auto;padding:0 16px}h1{font-size:1.45rem;margin:.2rem 0 1rem}.card{border:1px solid #8886;border-radius:12px;padding:16px;margin:12px 0}label{display:block;margin:.55rem 0}.row{display:flex;gap:14px;flex-wrap:wrap;align-items:center}.row>*{flex:1 1 170px}input[type=file],select,button{font:inherit}button{padding:.7rem 1rem;border-radius:8px;border:1px solid #888;cursor:pointer}button.primary{font-weight:700}button:disabled{opacity:.45;cursor:not-allowed}canvas{width:min(100%,768px);image-rendering:pixelated;border:1px solid #777;background:white;display:block}.small{opacity:.75;font-size:.9rem}#status{white-space:pre-wrap;font-family:ui-monospace,monospace}.range{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:center}.range input{width:100%}a{color:inherit}
</style></head><body>
<h1>SES / OTA5901 Ч image uploader v13</h1>
<div class="card">
  <label>Image file <input id="file" type="file" accept="image/png,image/jpeg,image/webp,image/bmp,image/gif"></label>
  <div class="row">
    <label>Scale
      <select id="scale"><option value="contain">Fit / letterbox</option><option value="cover">Fill / crop</option><option value="stretch">Stretch</option></select>
    </label>
    <label>Framebuffer packing
      <select id="packing">
        <option value="page-msb">Page 8px vertical / MSB top (diagnostic)</option>
        <option value="page-lsb">Page 8px vertical / LSB top (diagnostic)</option>
        <option value="row-msb">Row 8px horizontal / MSB left (diagnostic)</option>
        <option value="row-lsb" selected>Row 8px horizontal / LSB left (captured native)</option>
      </select>
    </label>
    <label><input id="dither" type="checkbox" checked> FloydЦSteinberg dithering</label>
    <label><input id="invert" type="checkbox"> Invert black/white</label>
  </div>
  <label class="range"><span>Threshold</span><span id="tv">150</span><input id="threshold" type="range" min="0" max="255" value="150"></label>
  <p class="small">The conversion runs in your browser. The controller receives exactly 4096 bytes (256?128, 1 bpp).</p>
</div>
<div class="card"><canvas id="preview" width="256" height="128"></canvas></div>
<div class="card row">
  <button id="send" class="primary" disabled>Upload &amp; display</button>
  <button id="white">Make white</button>
  <button id="black">Make black</button>
</div>
<div class="card">
  <b>Mapping diagnostics</b>
  <div class="row">
    <button id="vline">Vertical line x=0</button>
    <button id="hline">Horizontal line y=0</button>
    <button id="asym">Asymmetric mapping test</button>
  </div>
</div>
<div class="card"><div id="status">Choose an image.</div></div>
<p class="small"><a href="/">Back to ESPHome web UI</a></p>
<script>
const W=256,H=128,SZ=W*H/8;
const file=document.getElementById('file'), scale=document.getElementById('scale'), packing=document.getElementById('packing');
const dither=document.getElementById('dither'), invert=document.getElementById('invert');
const threshold=document.getElementById('threshold'), tv=document.getElementById('tv');
const canvas=document.getElementById('preview'), ctx=canvas.getContext('2d',{willReadFrequently:true});
const send=document.getElementById('send'), status=document.getElementById('status');
let img=null, raw=null;
function msg(x){status.textContent=x}
function drawSource(){
  ctx.save(); ctx.fillStyle='#fff'; ctx.fillRect(0,0,W,H);
  if(!img){ctx.restore();return}
  const iw=img.naturalWidth, ih=img.naturalHeight, m=scale.value;
  let dx=0,dy=0,dw=W,dh=H;
  if(m!=='stretch'){
    const k=(m==='cover')?Math.max(W/iw,H/ih):Math.min(W/iw,H/ih);
    dw=iw*k; dh=ih*k; dx=(W-dw)/2; dy=(H-dh)/2;
  }
  ctx.imageSmoothingEnabled=true; ctx.imageSmoothingQuality='high';
  ctx.drawImage(img,dx,dy,dw,dh); ctx.restore();
}
function clearPixel(buf,x,y){
  let idx,mask;
  switch(packing.value){
    case 'page-lsb': idx=(y>>3)*W+x; mask=1<<(y&7); break;
    case 'row-msb':  idx=y*(W>>3)+(x>>3); mask=1<<(7-(x&7)); break;
    case 'row-lsb':  idx=y*(W>>3)+(x>>3); mask=1<<(x&7); break;
    case 'page-msb':
    default:         idx=(y>>3)*W+x; mask=1<<(7-(y&7)); break;
  }
  buf[idx] &= ~mask;
}
function convert(){
  drawSource(); if(!img){raw=null;send.disabled=true;return}
  const im=ctx.getImageData(0,0,W,H), px=im.data, th=+threshold.value;
  const g=new Float32Array(W*H);
  for(let i=0,p=0;i<g.length;i++,p+=4) g[i]=.2126*px[p]+.7152*px[p+1]+.0722*px[p+2];
  const bw=new Uint8Array(W*H);
  if(dither.checked){
    for(let y=0;y<H;y++) for(let x=0;x<W;x++){
      const i=y*W+x, old=g[i], nv=old<th?0:255, e=old-nv; bw[i]=nv;
      if(x+1<W) g[i+1]+=e*7/16;
      if(y+1<H){ if(x>0) g[i+W-1]+=e*3/16; g[i+W]+=e*5/16; if(x+1<W) g[i+W+1]+=e/16; }
    }
  } else for(let i=0;i<g.length;i++) bw[i]=g[i]<th?0:255;
  raw=new Uint8Array(SZ); raw.fill(0xFF);
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    const i=y*W+x; let black=bw[i]===0; if(invert.checked) black=!black;
    const v=black?0:255, p=i*4; px[p]=px[p+1]=px[p+2]=v; px[p+3]=255;
    if(black) clearPixel(raw,x,y);
  }
  ctx.putImageData(im,0,0); send.disabled=false;
  msg(`${img.naturalWidth}?${img.naturalHeight} ? 256?128 / 4096 bytes ready`);
}
file.addEventListener('change',()=>{
  const f=file.files&&file.files[0]; if(!f)return;
  const u=URL.createObjectURL(f), n=new Image();
  n.onload=()=>{if(img&&img._url)URL.revokeObjectURL(img._url); n._url=u; img=n; convert()};
  n.onerror=()=>{URL.revokeObjectURL(u);msg('Cannot decode this image.');}; n.src=u;
});
for(const e of [scale,packing,dither,invert]) e.addEventListener('change',convert);
threshold.addEventListener('input',()=>{tv.textContent=threshold.value;convert()});
function syntheticFrame(kind){
  const b=new Uint8Array(SZ); b.fill(0xFF);
  const im=ctx.createImageData(W,H), p=im.data;
  for(let y=0;y<H;y++) for(let x=0;x<W;x++){
    let black=false;
    if(kind==='vline') black=(x===0);
    else if(kind==='hline') black=(y===0);
    else if(kind==='asym') black=((x<8&&y<8)||x===64||y===96||(x===y));
    const i=(y*W+x)*4, v=black?0:255;
    p[i]=p[i+1]=p[i+2]=v; p[i+3]=255;
    if(black) clearPixel(b,x,y);
  }
  ctx.putImageData(im,0,0);
  raw=b; send.disabled=false;
  msg(`Synthetic ${kind}, packing=${packing.value}, 4096 bytes ready`);
  return b;
}
async function postFrame(frame){
  if(!frame||frame.length!==SZ)throw new Error('frame size must be 4096 bytes');
  msg('Uploading 4096 bytesЕ');
  const r=await fetch('/ota5901/frame',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:frame});
  const t=await r.text(); if(!r.ok)throw new Error(t||`HTTP ${r.status}`); msg('Accepted.\n'+t);
}
send.addEventListener('click',async()=>{try{await postFrame(raw)}catch(e){msg('ERROR: '+e.message)}});
document.getElementById('white').addEventListener('click',async()=>{try{const b=new Uint8Array(SZ);b.fill(255);await postFrame(b)}catch(e){msg('ERROR: '+e.message)}});
document.getElementById('black').addEventListener('click',async()=>{try{const b=new Uint8Array(SZ);b.fill(0);await postFrame(b)}catch(e){msg('ERROR: '+e.message)}});
for(const id of ['vline','hline','asym']) document.getElementById(id).addEventListener('click',async()=>{try{await postFrame(syntheticFrame(id))}catch(e){msg('ERROR: '+e.message)}});
drawSource();
</script></body></html>)OTAPAGE";

class OTA5901ImageWebHandler : public AsyncWebHandler {
 public:
  explicit OTA5901ImageWebHandler(OTA5901Display *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() == HTTP_GET)  return request->url() == "/ota5901";
    if (request->method() == HTTP_POST) return request->url() == "/ota5901/frame";
    return false;
  }

  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                  size_t index, size_t total) override {
    if (request->url() != "/ota5901/frame") return;
    if (index == 0) {
      this->upload_started_ = true;
      this->upload_ok_ = this->parent_->web_begin_frame_upload(total);
    }
    if (this->upload_ok_)
      this->upload_ok_ = this->parent_->web_write_frame_chunk(index, data, len, total);
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    if (request->method() == HTTP_GET) {
      auto *response = request->beginResponse(
          200, "text/html",
          reinterpret_cast<const uint8_t *>(OTA5901_IMAGE_PAGE),
          sizeof(OTA5901_IMAGE_PAGE) - 1);
      response->addHeader("Cache-Control", "no-store");
      request->send(response);
      return;
    }
    bool ok = false;
    if (this->upload_started_ && this->upload_ok_)
      ok = this->parent_->web_finish_frame_upload();
    else
      this->parent_->web_abort_frame_upload();
    this->upload_started_ = false;
    this->upload_ok_ = false;
    if (ok)
      request->send(200, "application/json", "{\"ok\":true,\"bytes\":4096}");
    else
      request->send(400, "application/json",
                    "{\"ok\":false,\"error\":\"expected exactly 4096 raw bytes\"}");
  }

  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  OTA5901Display *parent_;
  bool upload_started_{false};
  bool upload_ok_{false};
};

// ============================================================================
// Setup
// ============================================================================
void OTA5901Display::setup() {
  ESP_LOGCONFIG(TAG, "Setting up OTA5901 display...");
  this->setup_pins_();
  this->attach_te_interrupt_();
  if (this->is_failed()) return;
  this->init_internal_(FRAMEBUFFER_SIZE);
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Framebuffer allocation failed");
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Framebuffer allocated: %u bytes",
           static_cast<unsigned>(FRAMEBUFFER_SIZE));
  // HA-службы и веб-хендлер регистрируютс€ лениво в loop().
}

void OTA5901Display::setup_pins_() {
  pinMode(this->sd_pin_, OUTPUT);
  pinMode(this->sclk_pin_, OUTPUT);
  pinMode(this->cs_pin_, OUTPUT);
  pinMode(this->reset_pin_, OUTPUT);
  pinMode(this->xclk_pin_, OUTPUT);
  if (this->has_te_pin_) pinMode(this->te_pin_, INPUT);

  digitalWrite(this->cs_pin_, HIGH);
  digitalWrite(this->sclk_pin_, LOW);
  digitalWrite(this->sd_pin_, LOW);
  digitalWrite(this->reset_pin_, HIGH);
  digitalWrite(this->xclk_pin_, LOW);
}

// ============================================================================
// –егистраци€ служб HA
// ============================================================================
void OTA5901Display::register_ha_services_() {
  if (this->ha_services_registered_) return;
  if (api::global_api_server == nullptr) return;  // API ещЄ не подн€т

  this->register_service(
      &OTA5901Display::on_upload_frame_b64,
      "upload_frame_b64",
      {"frame_b64", "invert"});
  this->register_service(
      &OTA5901Display::on_fill_solid,
      "fill_solid",
      {"black"});

  this->ha_services_registered_ = true;
  ESP_LOGI(TAG, "HA services registered: upload_frame_b64, fill_solid");
}

// ============================================================================
// ќбработчики служб HA
// ============================================================================
bool OTA5901Display::apply_frame_raw(const uint8_t *data, size_t len, bool invert) {
  if (this->is_failed() || this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Display not ready");
    return false;
  }
  if (len != FRAMEBUFFER_SIZE) {
    ESP_LOGE(TAG, "Invalid frame size: %u, expected %u",
             static_cast<unsigned>(len),
             static_cast<unsigned>(FRAMEBUFFER_SIZE));
    return false;
  }
  if (this->web_upload_in_progress_.load()) {
    ESP_LOGW(TAG, "Web upload in progress; HA frame ignored");
    return false;
  }
  if (invert) {
    for (size_t i = 0; i < FRAMEBUFFER_SIZE; i++)
      this->buffer_[i] = static_cast<uint8_t>(~data[i]);
  } else {
    memcpy(this->buffer_, data, FRAMEBUFFER_SIZE);
  }
  this->queue_refresh_();
  return true;
}

void OTA5901Display::on_upload_frame_b64(std::string frame_b64, bool invert) {
  ESP_LOGI(TAG, "HA upload_frame_b64: b64_len=%u, invert=%s",
           static_cast<unsigned>(frame_b64.size()),
           invert ? "true" : "false");

  std::vector<uint8_t> raw = base64_decode(frame_b64.c_str(), frame_b64.size());
  if (raw.empty()) {
    ESP_LOGE(TAG, "base64 decode failed");
    return;
  }
  if (this->apply_frame_raw(raw.data(), raw.size(), invert)) {
    ESP_LOGI(TAG, "Frame from HA queued for display");
  }
}

void OTA5901Display::on_fill_solid(bool black) {
  if (this->is_failed() || this->buffer_ == nullptr) return;
  if (this->web_upload_in_progress_.load()) {
    ESP_LOGW(TAG, "Web upload in progress; fill_solid ignored");
    return;
  }
  memset(this->buffer_, black ? 0x00 : 0xFF, FRAMEBUFFER_SIZE);
  this->queue_refresh_();
  ESP_LOGI(TAG, "Filled %s", black ? "black" : "white");
}

// ============================================================================
// XCLK
// ============================================================================
bool OTA5901Display::start_xclk_() {
  if (this->xclk_started_) return true;
#ifdef USE_ESP8266
  const uint32_t cpu_hz = ESP.getCpuFreqMHz() * 1000000UL;
  const uint32_t denom = 2UL * this->xclk_frequency_;
  const uint32_t half_cycles = (cpu_hz + denom / 2UL) / denom;
  if (half_cycles == 0) {
    ESP_LOGE(TAG, "Invalid XCLK frequency %" PRIu32 " Hz", this->xclk_frequency_);
    return false;
  }
  pinMode(this->xclk_pin_, OUTPUT);
  const int ok = startWaveformClockCycles(this->xclk_pin_, half_cycles, half_cycles, 0);
  if (!ok) {
    ESP_LOGE(TAG, "Could not start XCLK waveform on GPIO%u", this->xclk_pin_);
    return false;
  }
  const float actual = static_cast<float>(cpu_hz) / static_cast<float>(2UL * half_cycles);
  ESP_LOGI(TAG, "XCLK started on GPIO%u: requested %" PRIu32 " Hz, actual ~%.2f Hz",
           this->xclk_pin_, this->xclk_frequency_, actual);
  this->xclk_started_ = true;
  return true;
#elif defined(USE_ESP32)
  const uint32_t apb_hz = 80000000UL;
  const uint32_t freq = this->xclk_frequency_;
  ledc_timer_bit_t res_bits = LEDC_TIMER_1_BIT;
  uint32_t div_num = 1;
  bool found = false;
  for (uint8_t bits = 1; bits <= 14; bits++) {
    const uint32_t denom = (1UL << bits) * 2UL;
    const uint32_t d = (apb_hz + freq * denom / 2) / (freq * denom);
    if (d >= 1 && d <= 1024) {
      res_bits = static_cast<ledc_timer_bit_t>(bits);
      div_num = d; found = true; break;
    }
  }
  if (!found) {
    ESP_LOGE(TAG, "Cannot synthesize %" PRIu32 " Hz XCLK via LEDC", freq);
    return false;
  }
  this->xclk_ledc_channel_ = 0;
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = res_bits;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = freq;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK) {
    ESP_LOGE(TAG, "ledc_timer_config failed for %" PRIu32 " Hz", freq);
    return false;
  }
  ledc_channel_config_t ch = {};
  ch.gpio_num = this->xclk_pin_;
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.channel = static_cast<ledc_channel_t>(this->xclk_ledc_channel_);
  ch.timer_sel = LEDC_TIMER_0;
  ch.duty = (1U << res_bits) / 2;
  ch.hpoint = 0;
  if (ledc_channel_config(&ch) != ESP_OK) {
    ESP_LOGE(TAG, "ledc_channel_config failed on GPIO%u", this->xclk_pin_);
    return false;
  }
  const float actual = static_cast<float>(apb_hz) /
                       static_cast<float>(div_num * (1UL << res_bits));
  ESP_LOGI(TAG, "XCLK started on GPIO%u via LEDC: requested %" PRIu32 " Hz, actual ~%.2f Hz",
           this->xclk_pin_, freq, actual);
  this->xclk_started_ = true;
  return true;
#else
  ESP_LOGE(TAG, "Unsupported platform for XCLK generation");
  return false;
#endif
}

// ============================================================================
// SPI 9-бит (A0 + 8 data)
// ============================================================================
inline void OTA5901Display::spi_delay_() { delayMicroseconds(this->spi_half_period_us_); }

void OTA5901Display::send9_no_cs_(bool a0, uint8_t value) {
  pinMode(this->sd_pin_, OUTPUT);
  digitalWrite(this->sclk_pin_, LOW);
  digitalWrite(this->sd_pin_, a0 ? HIGH : LOW);
  this->spi_delay_();
  digitalWrite(this->sclk_pin_, HIGH);
  this->spi_delay_();
  digitalWrite(this->sclk_pin_, LOW);
  for (int bit = 7; bit >= 0; --bit) {
    digitalWrite(this->sd_pin_, (value & (1U << bit)) ? HIGH : LOW);
    this->spi_delay_();
    digitalWrite(this->sclk_pin_, HIGH);
    this->spi_delay_();
    digitalWrite(this->sclk_pin_, LOW);
  }
}

void OTA5901Display::write_word_(bool a0, uint8_t value) {
  const uint32_t sd_mask   = 1UL << this->sd_pin_;
  const uint32_t sclk_mask = 1UL << this->sclk_pin_;
  const uint32_t cs_mask   = 1UL << this->cs_pin_;
  const uint32_t half_us = this->spi_half_period_us_;
#ifdef USE_ESP8266
  const uint32_t half_cycles = ESP.getCpuFreqMHz() * half_us;
  auto wait = [](uint32_t cycles) {
    const uint32_t start = ESP.getCycleCount();
    while (static_cast<uint32_t>(ESP.getCycleCount() - start) < cycles) {}
  };
  gpio_clr_mask(sclk_mask); gpio_clr_mask(cs_mask); wait(half_cycles);
  auto send_bit = [&](bool one) {
    if (one) gpio_set_mask(sd_mask); else gpio_clr_mask(sd_mask);
    wait(half_cycles);
    gpio_set_mask(sclk_mask); wait(half_cycles);
    gpio_clr_mask(sclk_mask);
  };
  send_bit(a0);
  for (int bit = 7; bit >= 0; --bit) send_bit((value & (1U << bit)) != 0);
  wait(half_cycles); gpio_set_mask(cs_mask); wait(half_cycles);
#else
  gpio_clr_mask(sclk_mask); gpio_clr_mask(cs_mask);
  if (half_us != 0) delayMicroseconds(half_us);
  auto send_bit = [&](bool one) {
    if (one) gpio_set_mask(sd_mask); else gpio_clr_mask(sd_mask);
    if (half_us != 0) delayMicroseconds(half_us);
    gpio_set_mask(sclk_mask);
    if (half_us != 0) delayMicroseconds(half_us);
    gpio_clr_mask(sclk_mask);
  };
  send_bit(a0);
  for (int bit = 7; bit >= 0; --bit) send_bit((value & (1U << bit)) != 0);
  if (half_us != 0) delayMicroseconds(half_us);
  gpio_set_mask(cs_mask);
  if (half_us != 0) delayMicroseconds(half_us);
#endif
}

void OTA5901Display::cmd_data_(uint8_t command, const uint8_t *data, size_t len) {
  this->cmd_(command);
  for (size_t i = 0; i < len; i++) this->data_(data[i]);
}

uint16_t OTA5901Display::read_reg16_(uint8_t command) {
  digitalWrite(this->cs_pin_, LOW);
  this->spi_delay_();
  this->send9_no_cs_(false, command);
  pinMode(this->sd_pin_, INPUT);
  this->spi_delay_();
  digitalWrite(this->sclk_pin_, HIGH);
  this->spi_delay_();
  (void) digitalRead(this->sd_pin_);
  digitalWrite(this->sclk_pin_, LOW);
  uint16_t value = 0;
  for (int i = 0; i < 16; i++) {
    this->spi_delay_();
    digitalWrite(this->sclk_pin_, HIGH);
    this->spi_delay_();
    value = static_cast<uint16_t>((value << 1) | (digitalRead(this->sd_pin_) ? 1 : 0));
    digitalWrite(this->sclk_pin_, LOW);
  }
  pinMode(this->sd_pin_, OUTPUT);
  digitalWrite(this->sd_pin_, LOW);
  this->spi_delay_();
  digitalWrite(this->cs_pin_, HIGH);
  this->spi_delay_();
  return value;
}

uint16_t OTA5901Display::read_status_4a_() {
  const uint8_t one = 0x01, zero = 0x00;
  this->cmd_data_(0xF1, &one, 1);
  const uint16_t status = this->read_reg16_(0x4A);
  this->cmd_data_(0xF1, &zero, 1);
  return status;
}

bool OTA5901Display::wait_ready_() {
  const uint32_t started = millis();
  uint16_t last = 0xFFFF;
  uint32_t polls = 0;
  while (static_cast<uint32_t>(millis() - started) < this->ready_timeout_ms_) {
    const uint16_t status = this->read_status_4a_();
    if (status != last) { ESP_LOGVV(TAG, "Status 0x4A = 0x%04X", status); last = status; }
    if (status == 0x00C0) return true;
    polls++;
    if ((polls & 0x0F) == 0) App.feed_wdt();
    delay(1);
  }
  ESP_LOGE(TAG, "Ready timeout after %" PRIu32 " ms, last status=0x%04X",
           this->ready_timeout_ms_, last);
  return false;
}

void OTA5901Display::set_ram_address_(uint16_t address) {
  const uint8_t p[2] = {static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
  this->cmd_data_(0x2A, p, sizeof(p));
}

void OTA5901Display::write_ram_repeat_(uint16_t address, uint8_t value, size_t len) {
  while (len > 0) {
    const size_t chunk = len > 256 ? 256 : len;
    this->set_ram_address_(address);
    this->cmd_(0x2C);
    for (size_t i = 0; i < chunk; i++) this->data_(value);
    address = static_cast<uint16_t>(address + chunk);
    len -= chunk;
    App.feed_wdt();
    yield();
  }
}

void OTA5901Display::minimal_init_() {
  digitalWrite(this->cs_pin_, HIGH);
  digitalWrite(this->sclk_pin_, LOW);
  digitalWrite(this->sd_pin_, LOW);
  digitalWrite(this->reset_pin_, HIGH);
  if (!this->start_xclk_()) return;
  delay(10);
  digitalWrite(this->reset_pin_, LOW);
  delayMicroseconds(50);
  digitalWrite(this->reset_pin_, HIGH);
  delay(50);
  this->cmd_(0x11);
  this->write_ram_repeat_(0x0000, 0xFF, FRAMEBUFFER_SIZE);
  delay(10);
  { const uint8_t p[] = {0x0C, 0x00, 0x00, 0x00}; this->cmd_data_(0x4C, p, sizeof(p)); }
  delay(4);
  { const uint8_t p[] = {0xFF, 0x00, 0x7F}; this->cmd_data_(0x4D, p, sizeof(p)); }
  delay(1);
  { const uint8_t p[] = {0x60}; this->cmd_data_(0x4E, p, sizeof(p)); }
  delay(1);
}

bool OTA5901Display::ensure_initialized_() {
  if (this->controller_initialized_) return true;
  ESP_LOGI(TAG, "Cold-initializing OTA5901 once");
  this->minimal_init_();
  if (!this->xclk_started_) return false;
  delay(500);
  this->controller_initialized_ = true;
  if (this->has_te_pin_) this->te_wait_sequence_ = this->te_event_counter_;
  ESP_LOGI(TAG, "OTA5901 initialization complete");
  return true;
}

void OTA5901Display::attach_te_interrupt_() {
  if (!this->has_te_pin_) return;
#ifdef USE_ESP8266
  if (this->te_pin_ == 16) {
    ESP_LOGE(TAG, "GPIO16 cannot be used for TE interrupt on ESP8266");
    this->mark_failed();
    return;
  }
#endif
#ifdef USE_ESP32
  if (this->te_pin_ >= 34 && this->te_pin_ <= 39)
    ESP_LOGW(TAG, "TE on GPIO%u has no internal pull-up", this->te_pin_);
#endif
  OTA5901Display::te_isr_instance_ = this;
  this->te_event_counter_ = 0;
  this->te_wait_sequence_ = 0;
  pinMode(this->te_pin_, INPUT);
  attachInterrupt(digitalPinToInterrupt(this->te_pin_),
                  OTA5901Display::te_isr_trampoline_, FALLING);
  this->te_interrupt_attached_ = true;
  ESP_LOGI(TAG, "TE interrupt attached on GPIO%u: FALLING edge", this->te_pin_);
}

void IRAM_ATTR OTA5901Display::te_isr_trampoline_() {
  OTA5901Display *instance = OTA5901Display::te_isr_instance_;
  if (instance != nullptr) instance->te_event_counter_++;
}

void OTA5901Display::queue_refresh_() {
  this->refresh_pending_.store(true);
  this->refresh_retry_count_ = 0;
  if (this->has_te_pin_) this->te_wait_sequence_ = this->te_event_counter_;
}

void OTA5901Display::write_frame_() {
  App.feed_wdt();
  this->set_ram_address_(0x0000);
  this->cmd_(0x2C);
  for (size_t i = 0; i < FRAMEBUFFER_SIZE; i++) {
    this->data_(this->buffer_[i]);
    if ((i & 0x3FF) == 0) App.feed_wdt();
  }
  App.feed_wdt();
}

void OTA5901Display::service_refresh_() {
  if (!this->refresh_pending_.load() || this->is_failed() || this->buffer_ == nullptr) return;
  if (this->web_upload_in_progress_.load()) return;
  if (!this->ensure_initialized_()) {
    ESP_LOGE(TAG, "Cannot initialize OTA5901 for refresh");
    this->refresh_pending_.store(false);
    return;
  }
  uint32_t edge_sequence = this->te_event_counter_;
  if (this->has_te_pin_) {
    if (edge_sequence == this->te_wait_sequence_) return;
    this->te_wait_sequence_ = edge_sequence;
    if (this->te_delay_us_ != 0) delayMicroseconds(this->te_delay_us_);
  }
  const uint32_t event_at_start = this->te_event_counter_;
  const uint32_t t0 = micros();
  this->write_frame_();
  const uint32_t elapsed = static_cast<uint32_t>(micros() - t0);
  const uint32_t event_at_end = this->te_event_counter_;
  const bool te_low_after = !this->has_te_pin_ || digitalRead(this->te_pin_) == LOW;
  const bool crossed_window = this->has_te_pin_ &&
                              ((event_at_end != event_at_start) || !te_low_after);
  ESP_LOGI(TAG, "OTA5901 frame: %.1f ms, TE event=%u->%u, TE after=%s%s",
           elapsed / 1000.0f,
           static_cast<unsigned>(event_at_start),
           static_cast<unsigned>(event_at_end),
           te_low_after ? "LOW" : "HIGH",
           crossed_window ? ", crossed window" : "");
  if (crossed_window) {
    if (this->refresh_retry_count_ < 2) {
      this->refresh_retry_count_++;
      this->te_wait_sequence_ = event_at_end;
      return;
    }
    this->refresh_pending_.store(false);
    this->refresh_retry_count_ = 0;
    return;
  }
  this->refresh_pending_.store(false);
  this->refresh_retry_count_ = 0;
  this->last_hash_ = this->framebuffer_hash_();
  this->have_last_hash_ = true;
}

uint32_t OTA5901Display::framebuffer_hash_() const {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < FRAMEBUFFER_SIZE; i++) {
    h ^= this->buffer_[i];
    h *= 16777619UL;
  }
  return h;
}

// ============================================================================
// Debug helpers
// ============================================================================
bool OTA5901Display::debug_render_lambda() {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  this->clear(); this->do_update_(); this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_fill_white() {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  memset(this->buffer_, 0xFF, FRAMEBUFFER_SIZE); this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_fill_black() {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  memset(this->buffer_, 0x00, FRAMEBUFFER_SIZE); this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_checkerboard(uint8_t cell) {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  if (cell == 0) cell = 1;
  memset(this->buffer_, 0xFF, FRAMEBUFFER_SIZE);
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      if ((((x / cell) + (y / cell)) & 1) != 0) {
        const size_t index = static_cast<size_t>(y) * (WIDTH >> 3) + static_cast<size_t>(x >> 3);
        this->buffer_[index] &= static_cast<uint8_t>(~(1U << (x & 7)));
      }
    }
    if ((y & 0x0F) == 0) App.feed_wdt();
  }
  this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_raw_fill(uint8_t value) {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  memset(this->buffer_, value, FRAMEBUFFER_SIZE); this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_test_pattern() {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  memset(this->buffer_, 0xFF, FRAMEBUFFER_SIZE);
  auto black_pixel = [this](int x, int y) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    const size_t index = static_cast<size_t>(y) * (WIDTH >> 3) + static_cast<size_t>(x >> 3);
    this->buffer_[index] &= static_cast<uint8_t>(~(1U << (x & 7)));
  };
  for (int x = 0; x < WIDTH; x++) {
    black_pixel(x, 0); black_pixel(x, HEIGHT - 1);
    if ((x % 16) == 0) for (int y = 0; y < HEIGHT; y++) black_pixel(x, y);
  }
  for (int y = 0; y < HEIGHT; y++) {
    black_pixel(0, y); black_pixel(WIDTH - 1, y);
    if ((y % 16) == 0) for (int x = 0; x < WIDTH; x++) black_pixel(x, y);
  }
  for (int i = 0; i < HEIGHT; i++) { black_pixel(i, i); black_pixel(WIDTH - 1 - i, i); }
  this->queue_refresh_();
  return true;
}
bool OTA5901Display::debug_reinit_current() {
  if (this->is_failed() || this->buffer_ == nullptr) return false;
  this->controller_initialized_ = false;
  this->queue_refresh_();
  return true;
}
uint16_t OTA5901Display::debug_read_status() {
  if (!this->start_xclk_()) return 0xFFFF;
  const uint16_t value = this->read_status_4a_();
  ESP_LOGI(TAG, "Manual status 0x4A = 0x%04X", value);
  return value;
}

// ============================================================================
// Web upload
// ============================================================================
bool OTA5901Display::web_begin_frame_upload(size_t total) {
  if (this->is_failed() || this->buffer_ == nullptr || total != FRAMEBUFFER_SIZE) {
    ESP_LOGW(TAG, "Web frame rejected: total=%u", static_cast<unsigned>(total));
    this->web_upload_in_progress_.store(false);
    this->web_upload_received_.store(0);
    return false;
  }
  this->web_upload_in_progress_.store(true);
  this->web_upload_received_.store(0);
  this->web_upload_started_ms_ = millis();
  return true;
}
bool OTA5901Display::web_write_frame_chunk(size_t index, const uint8_t *data,
                                           size_t len, size_t total) {
  if (!this->web_upload_in_progress_.load() || this->buffer_ == nullptr ||
      total != FRAMEBUFFER_SIZE || index != this->web_upload_received_.load() ||
      index + len > FRAMEBUFFER_SIZE) {
    this->web_upload_in_progress_.store(false);
    return false;
  }
  if (len > 0) memcpy(this->buffer_ + index, data, len);
  this->web_upload_received_.fetch_add(len);
  return true;
}
bool OTA5901Display::web_finish_frame_upload() {
  const bool ok = this->web_upload_in_progress_.load() &&
                  this->web_upload_received_.load() == FRAMEBUFFER_SIZE;
  this->web_upload_in_progress_.store(false);
  this->web_upload_received_.store(0);
  if (!ok) return false;
  this->queue_refresh_();
  ESP_LOGI(TAG, "Web frame accepted");
  return true;
}
void OTA5901Display::web_abort_frame_upload() {
  this->web_upload_in_progress_.store(false);
  this->web_upload_received_.store(0);
}

// ============================================================================
// Main loop / update
// ============================================================================
void OTA5901Display::loop() {
  if (!this->ha_services_registered_) {
    this->register_ha_services_();
  }
  if (!this->web_handler_registered_) {
    auto *web_base = web_server_base::global_web_server_base;
    if (web_base != nullptr) {
      web_base->add_handler(new OTA5901ImageWebHandler(this));
      this->web_handler_registered_ = true;
      ESP_LOGI(TAG, "Image uploader registered at /ota5901");
    }
  }
  if (this->web_upload_in_progress_.load() &&
      (millis() - this->web_upload_started_ms_) > this->upload_timeout_ms_) {
    ESP_LOGW(TAG, "Web upload timed out");
    this->web_abort_frame_upload();
  }
  this->service_refresh_();
}

void OTA5901Display::update() {
  if (this->is_failed() || this->buffer_ == nullptr) return;
  if (this->web_upload_in_progress_.load()) return;
  this->do_update_();
  const uint32_t hash = this->framebuffer_hash_();
  if (this->skip_unchanged_ && this->have_last_hash_ && hash == this->last_hash_) {
    ESP_LOGV(TAG, "Framebuffer unchanged");
    return;
  }
  this->queue_refresh_();
}

void OTA5901Display::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || this->buffer_ == nullptr) return;
  const size_t index = static_cast<size_t>(y) * (WIDTH >> 3) + static_cast<size_t>(x >> 3);
  const uint8_t mask = static_cast<uint8_t>(1U << (x & 7));
  if (color.is_on()) this->buffer_[index] &= static_cast<uint8_t>(~mask);
  else              this->buffer_[index] |= mask;
}

void OTA5901Display::dump_config() {
  ESP_LOGCONFIG(TAG, "OTA5901 / HT023YFB:");
  ESP_LOGCONFIG(TAG, "  SD: GPIO%u, SCLK: GPIO%u, CS: GPIO%u, RST: GPIO%u, XCLK: GPIO%u",
                this->sd_pin_, this->sclk_pin_, this->cs_pin_,
                this->reset_pin_, this->xclk_pin_);
  ESP_LOGCONFIG(TAG, "  XCLK: %" PRIu32 " Hz", this->xclk_frequency_);
  if (this->has_te_pin_)
    ESP_LOGCONFIG(TAG, "  TE: GPIO%u", this->te_pin_);
  ESP_LOGCONFIG(TAG, "  Framebuffer: 256x128, 1 bpp, 4096 bytes, row-lsb");
  ESP_LOGCONFIG(TAG, "  HA services: upload_frame_b64, fill_solid");
}

float OTA5901Display::get_setup_priority() const { return setup_priority::PROCESSOR; }

}  // namespace ota5901
}  // namespace esphome