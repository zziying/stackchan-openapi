/*
 * StackChan HTTP API Firmware
 * Turns StackChan into a WiFi-controlled robot with HTTP endpoints
 * for face expressions, servo, camera, audio, and touch.
 *
 * Hardware: M5Stack StackChan (CoreS3 ESP32-S3)
 * All AI/ML runs on a host computer — this firmware is the "dumb terminal".
 *
 * API endpoints:
 *   GET /           — control panel
 *   GET /face       — set expression (neutral/happy/sad/angry/sleepy/doubt/love/eyeroll)
 *   GET /servo      — move servos (yaw, pitch, speed)
 *   GET /camera     — capture JPEG photo
 *   GET /status     — device status JSON
 *   GET /home       — center servos
 *   GET /touch      — touch sensor readings
 *   GET /record     — record audio (WAV)
 *   GET /stream     — stream mic PCM to host via TCP
 *   GET /play       — play WAV from URL (streaming, up to 2MB/62s)
 *   GET /speech     — display subtitle text
 *   GET /volume     — mic volume level
 */
#include <M5StackChan.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_camera.h>
#include <Avatar.h>
#include <Face.h>
#include <HTTPClient.h>

#include "config.h"

using namespace m5avatar;

// ============ Custom Face ============
// 7 expressions: happy ^^, sad, love (heart eyes + blush), angry,
// sleepy (Zzz), doubt (?), eyeroll
// Base: rectangular eyes + horizontal line mouth + no eyebrows

// Custom expressions beyond the library's built-in set.
// Rendered only when avatar is Neutral, so reverting to Neutral
// automatically restores the custom face.
String g_customExpr = "";

// Subtitle system (replaces the library's Balloon which is too small for CJK)
// Layout is done once in handleSpeech; render thread only reads static arrays.
const int SUB_MAX_LINES = 16;
char g_subLines[SUB_MAX_LINES][64];
int g_subLineChars[SUB_MAX_LINES];
volatile int g_subNLines = 0;
int g_subTotalChars = 0;
unsigned long g_speechStart = 0;
unsigned long g_speechDurMs = 0;

// UTF-8 line breaking: CJK (3 bytes) = 16px, ASCII = 8px, max line width 292px
static void buildSubtitle(const String &text) {
  g_subNLines = 0;
  memset(g_subLineChars, 0, sizeof(g_subLineChars));
  int li = 0, lineW = 0, lineLen = 0, total = 0;
  bool full = false;
  for (unsigned int i = 0; i + 1 <= text.length() && !full;) {
    uint8_t c = text[i];
    int clen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (i + clen > text.length()) break;
    int cw = (clen == 1) ? 8 : 16;
    if (lineW + cw > 292 || lineLen + clen > 60) {
      g_subLines[li][lineLen] = '\0';
      if (++li >= SUB_MAX_LINES) { full = true; break; }
      lineW = 0; lineLen = 0;
    }
    for (int k = 0; k < clen; k++) g_subLines[li][lineLen++] = text[i + k];
    g_subLineChars[li]++;
    total++;
    lineW += cw;
    i += clen;
  }
  if (!full) {
    g_subLines[li][lineLen] = '\0';
    li++;
  }
  g_subTotalChars = total;
  g_subNLines = (total > 0) ? li : 0;
}

// Bottom subtitle bar: 2 lines per page, auto-paging synced to speech duration
static void drawSubtitle(M5Canvas *spi, uint16_t fg) {
  int nLines = g_subNLines;
  if (nLines <= 0) return;
  int nPages = (nLines + 1) / 2;

  int page = nPages - 1;
  unsigned long durMs = g_speechDurMs > 0 ? g_speechDurMs : (unsigned long)g_subTotalChars * 250;
  unsigned long elapsed = millis() - g_speechStart;
  unsigned long acc = 0;
  for (int p = 0; p < nPages; p++) {
    int pc = g_subLineChars[p * 2] + (p * 2 + 1 < nLines ? g_subLineChars[p * 2 + 1] : 0);
    acc += (unsigned long)pc * durMs / (g_subTotalChars ? g_subTotalChars : 1);
    if (elapsed < acc) { page = p; break; }
  }

  spi->fillRoundRect(6, 168, 308, 66, 10, 0);
  spi->drawRoundRect(6, 168, 308, 66, 10, fg);
  spi->drawRoundRect(7, 169, 306, 64, 9, fg);
  spi->setFont(&fonts::efontCN_16);
  spi->setTextSize(1);
  spi->setTextColor(fg);
  spi->setTextDatum(TL_DATUM);
  bool hasL2 = (page * 2 + 1 < nLines);
  if (!hasL2) {
    spi->drawString(g_subLines[page * 2], 14, 192);
  } else {
    spi->drawString(g_subLines[page * 2], 14, 178);
    spi->drawString(g_subLines[page * 2 + 1], 14, 202);
  }
  if (nPages > 1) {
    for (int p = 0; p < nPages && p < 8; p++) {
      int dx = 302 - (nPages - 1 - p) * 10;
      if (p == page) spi->fillCircle(dx, 227, 2, fg);
      else spi->drawCircle(dx, 227, 2, fg);
    }
  }
  spi->setFont(&fonts::Font0);
}

// Manga-style anger mark
static void drawAngerMark(M5Canvas *spi, int ax, int ay, uint16_t col) {
  for (int dx = -1; dx <= 1; dx += 2) {
    for (int dy = -1; dy <= 1; dy += 2) {
      int ex = ax + dx * 9, ey = ay + dy * 9;
      spi->drawLine(ex, ey, ex - dx * 7, ey, col);
      spi->drawLine(ex, ey + dy, ex - dx * 7, ey + dy, col);
      spi->drawLine(ex, ey, ex, ey - dy * 7, col);
      spi->drawLine(ex + dx, ey, ex + dx, ey - dy * 7, col);
    }
  }
}

class CustomEye final : public Drawable {
  bool isLeft;
 public:
  CustomEye(bool isLeft) : isLeft(isLeft) {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    uint32_t cx = rect.getCenterX();
    uint32_t cy = rect.getCenterY();
    Expression exp = ctx->getExpression();
    Gaze g = isLeft ? ctx->getLeftGaze() : ctx->getRightGaze();
    float openRatio = isLeft ? ctx->getLeftEyeOpenRatio() : ctx->getRightEyeOpenRatio();
    int ox = g.getHorizontal() * 3;
    int oy = g.getVertical() * 3;
    uint16_t col = ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);

    bool customActive = (exp == Expression::Neutral) && g_customExpr.length() > 0;

    // Love: heart eyes + blush lines
    if (customActive && g_customExpr == "love") {
      spi->fillCircle(cx - 8, cy - 5, 10, col);
      spi->fillCircle(cx + 8, cy - 5, 10, col);
      spi->fillTriangle(cx - 17, cy - 2, cx + 17, cy - 2, cx, cy + 17, col);
      for (int i = 0; i < 3; i++) {
        int bx = cx - 10 + i * 7;
        spi->drawLine(bx, cy + 27, bx + 5, cy + 21, col);
        spi->drawLine(bx + 1, cy + 27, bx + 6, cy + 21, col);
      }
      return;
    }

    // Eyeroll: eye socket + pupil at top
    if (customActive && g_customExpr == "eyeroll") {
      int w = 36, h = 18;
      int x = cx - w / 2, y = cy - h / 2;
      spi->drawRect(x, y, w, h, col);
      spi->drawRect(x + 1, y + 1, w - 2, h - 2, col);
      spi->fillRect(cx - 5, y + 2, 10, 6, col);
      return;
    }

    // Happy: ^^ eyes
    if (exp == Expression::Happy && openRatio > 0.4f) {
      for (int i = 0; i < 3; i++) {
        spi->drawLine(cx + ox - 11, cy + oy + 5 + i, cx + ox, cy + oy - 6 + i, col);
        spi->drawLine(cx + ox, cy + oy - 6 + i, cx + ox + 11, cy + oy + 5 + i, col);
      }
      return;
    }

    int w = 28;
    int hBase = (exp == Expression::Angry) ? 7 : 10;
    int h = max(2, (int)(hBase * openRatio));
    if (exp == Expression::Sleepy) h = 3;
    if (exp == Expression::Doubt) oy += isLeft ? -3 : 2;
    int x = cx + ox - w / 2;
    int y = cy + oy - h / 2;
    spi->fillRect(x, y, w, h, col);
  }
};

class CustomMouth final : public Drawable {
 public:
  CustomMouth() {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    uint16_t col = ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
    Expression exp = ctx->getExpression();
    float breath = min(1.0f, ctx->getBreath());
    float openRatio = ctx->getMouthOpenRatio();
    int cx = rect.getLeft();
    int cy = rect.getTop() + (int)(breath * 2);

    // Lip sync: open mouth rectangle during speech
    if (openRatio > 0.15f) {
      int h = max(3, (int)(8 * openRatio));
      spi->fillRect(cx - 12, cy - h / 2, 24, h, col);
      return;
    }

    bool customActive = (exp == Expression::Neutral) && g_customExpr.length() > 0;

    if (customActive && g_customExpr == "love") {
      spi->fillArc(cx, cy - 19, 23, 25, 60, 120, col);
      return;
    }
    if (exp == Expression::Happy) {
      spi->fillArc(cx, cy - 28, 34, 36, 53, 127, col);
      return;
    }
    if (exp == Expression::Sad) {
      spi->fillArc(cx, cy + 28, 34, 36, 233, 307, col);
      return;
    }
    if (exp == Expression::Sleepy) {
      spi->fillEllipse(cx, cy, 6, 8, col);
      return;
    }

    int w = (exp == Expression::Doubt) ? 16 : 42;
    spi->fillRect(cx - w / 2, cy - 1, w, 2, col);
  }
};

class CustomBrow final : public Drawable {
  bool isLeft;
 public:
  CustomBrow(bool isLeft) : isLeft(isLeft) {}
  void draw(M5Canvas *spi, BoundingRect rect, DrawContext *ctx) override {
    Expression exp = ctx->getExpression();
    uint16_t col = ctx->getColorDepth() == 1 ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);

    // Subtitle bar (drawn once by right brow instance)
    if (!isLeft && g_subNLines > 0) {
      drawSubtitle(spi, col);
    }

    // Decorations (drawn once by right brow)
    if (!isLeft) {
      if (exp == Expression::Angry) drawAngerMark(spi, 272, 52, col);
      if (exp == Expression::Sleepy) {
        spi->setTextColor(col);
        spi->setTextSize(3); spi->drawString("Z", 264, 32);
        spi->setTextSize(2); spi->drawString("z", 250, 58);
        spi->setTextSize(1); spi->drawString("z", 240, 76);
        spi->setTextSize(1);
      }
      if (exp == Expression::Doubt) {
        spi->setTextColor(col);
        spi->setTextSize(3); spi->drawString("?", 262, 36);
        spi->setTextSize(1);
      }
    }

    if (exp == Expression::Neutral || exp == Expression::Happy || exp == Expression::Sleepy) return;
    int cx = rect.getCenterX();
    int cy = rect.getCenterY();
    int bw = 36;
    int innerTilt = 0;
    if (exp == Expression::Angry) {
      innerTilt = 10;
      cy += 4;
    }
    if (exp == Expression::Sad)    innerTilt = -8;
    if (exp == Expression::Doubt) {
      if (isLeft) { innerTilt = -6; cy -= 5; }
      else        { cy += 3; }
    }
    if (exp == Expression::Happy)  innerTilt = -5;
    int x0 = cx - bw / 2;
    int x1 = cx + bw / 2;
    int y0, y1;
    if (isLeft) {
      y0 = cy - innerTilt;
      y1 = cy + innerTilt;
    } else {
      y0 = cy + innerTilt;
      y1 = cy - innerTilt;
    }
    spi->drawLine(x0, y0, x1, y1, col);
    spi->drawLine(x0, y0 + 1, x1, y1 + 1, col);
  }
};

// StackChan camera pins (GC0308)
#define CAM_PIN_SIOD  12
#define CAM_PIN_SIOC  11
#define CAM_PIN_XCLK  -1
#define CAM_PIN_VSYNC 46
#define CAM_PIN_HREF  38
#define CAM_PIN_PCLK  45
#define CAM_PIN_D0    39
#define CAM_PIN_D1    40
#define CAM_PIN_D2    41
#define CAM_PIN_D3    42
#define CAM_PIN_D4    15
#define CAM_PIN_D5    16
#define CAM_PIN_D6    48
#define CAM_PIN_D7    47

WebServer server(80);
Avatar avatar;

// ============ State ============

String currentExprName = "neutral";
Expression baseExpr = Expression::Neutral;
unsigned long touchExprTime = 0;
bool touchExprActive = false;

unsigned long httpExprTime = 0;
bool httpExprActive = false;

// Double-tap detection
unsigned long lastValidTap = 0;
const unsigned long DOUBLE_TAP_WINDOW = 800;

// Shake detection (BMI270 accelerometer)
bool imuReady = false;
float prevAccelMag = 1.0f;
int shakeCount = 0;
unsigned long lastShakeCheck = 0;
unsigned long lastShakeReaction = 0;
const unsigned long SHAKE_COOLDOWN = 5000;
const float SHAKE_THRESHOLD = 0.8f;

// ============ Audio ============

static const int RECORD_SAMPLE_RATE = 16000;
static const int RECORD_CHANNELS = 1;
static const int RECORD_BITS = 16;
bool micActive = false;
bool speakerActive = false;
uint8_t* g_playBuf = nullptr;

// ============ Lip Sync ============
bool g_lipSyncActive = false;
int16_t* g_lipData = nullptr;
uint32_t g_lipSamples = 0;
uint32_t g_lipRate = 16000;
unsigned long g_lipStart = 0;
unsigned long g_lipLastUpdate = 0;
const float LIP_RMS_SCALE = 1800.0f;

void startMic() {
  if (speakerActive) {
    while (M5.Speaker.isPlaying()) { delay(1); }
    M5.Speaker.end();
    speakerActive = false;
  }
  if (!micActive) {
    M5.Mic.begin();
    micActive = true;
  }
}

void startSpeaker() {
  if (micActive) {
    while (M5.Mic.isRecording()) { delay(1); }
    M5.Mic.end();
    micActive = false;
  }
  if (!speakerActive) {
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);
    M5.Speaker.setChannelVolume(0, 255);
    speakerActive = true;
  }
}

void writeWavHeader(uint8_t* buf, uint32_t dataLen) {
  uint32_t fileSize = 36 + dataLen;
  buf[0]='R'; buf[1]='I'; buf[2]='F'; buf[3]='F';
  memcpy(buf+4, &fileSize, 4);
  buf[8]='W'; buf[9]='A'; buf[10]='V'; buf[11]='E';
  buf[12]='f'; buf[13]='m'; buf[14]='t'; buf[15]=' ';
  uint32_t fmtSize = 16;     memcpy(buf+16, &fmtSize, 4);
  uint16_t audioFmt = 1;     memcpy(buf+20, &audioFmt, 2);
  uint16_t channels = RECORD_CHANNELS; memcpy(buf+22, &channels, 2);
  uint32_t srate = RECORD_SAMPLE_RATE; memcpy(buf+24, &srate, 4);
  uint32_t byteRate = RECORD_SAMPLE_RATE * RECORD_CHANNELS * (RECORD_BITS/8);
  memcpy(buf+28, &byteRate, 4);
  uint16_t blockAlign = RECORD_CHANNELS * (RECORD_BITS/8);
  memcpy(buf+32, &blockAlign, 2);
  uint16_t bps = RECORD_BITS; memcpy(buf+34, &bps, 2);
  buf[36]='d'; buf[37]='a'; buf[38]='t'; buf[39]='a';
  memcpy(buf+40, &dataLen, 4);
}

// ============ Touch Callback ============

const int TOUCH_PORT = 7070;
const int VOICE_PORT = 7072;

void notifyCallback(const char* type, int port) {
  WiFiClient client;
  if (client.connect(CALLBACK_HOST, port)) {
    String body = String("{\"event\":\"touch\",\"type\":\"") + type + "\"}";
    client.println("POST /touch HTTP/1.1");
    client.printf("Host: %s\r\n", CALLBACK_HOST);
    client.println("Content-Type: application/json");
    client.printf("Content-Length: %d\r\n", body.length());
    client.println();
    client.print(body);
    client.stop();
  }
}

// ============ Camera ============

bool cameraReady = false;
String cameraError = "";

bool initCamera() {
  M5.In_I2C.release();

  camera_config_t config;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_pclk = CAM_PIN_PCLK;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 0;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.sccb_i2c_port = -1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    char buf[64];
    snprintf(buf, sizeof(buf), "esp_camera_init error: 0x%x (%s)", err, esp_err_to_name(err));
    cameraError = String(buf);
    return false;
  }
  cameraError = "none";
  return true;
}

// ============ HTTP Handlers ============

void handleRoot() {
  String html = "<html><head><title>StackChan API</title></head><body>";
  html += "<h1>StackChan HTTP API</h1>";
  html += "<h2>Face</h2>";
  html += "<a href='/face?expr=neutral'>Neutral</a> | ";
  html += "<a href='/face?expr=happy'>Happy</a> | ";
  html += "<a href='/face?expr=sad'>Sad</a> | ";
  html += "<a href='/face?expr=angry'>Angry</a> | ";
  html += "<a href='/face?expr=sleepy'>Sleepy</a> | ";
  html += "<a href='/face?expr=doubt'>Doubt</a> | ";
  html += "<a href='/face?expr=love'>Love</a> | ";
  html += "<a href='/face?expr=eyeroll'>Eyeroll</a>";
  html += "<h2>Servo</h2>";
  html += "<a href='/servo?yaw=0&pitch=450'>Center</a> | ";
  html += "<a href='/servo?yaw=600&pitch=450'>Look Left</a> | ";
  html += "<a href='/servo?yaw=-600&pitch=450'>Look Right</a>";
  html += "<h2>Camera</h2>";
  html += "<a href='/camera'>Capture</a>";
  html += "<h2>Voice</h2>";
  html += "<a href='/record?seconds=3'>Record 3s</a> | ";
  html += "<a href='/volume'>Volume</a>";
  html += "<p>/play?url=http://host/file.wav to play audio</p>";
  html += "<h2>Status</h2>";
  html += "<a href='/status'>Status</a>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleFace() {
  String expr = server.arg("expr");
  bool valid = true;
  g_customExpr = "";
  if (expr == "neutral") { avatar.setExpression(Expression::Neutral); baseExpr = Expression::Neutral; }
  else if (expr == "happy") { avatar.setExpression(Expression::Happy); baseExpr = Expression::Happy; }
  else if (expr == "sad") { avatar.setExpression(Expression::Sad); baseExpr = Expression::Sad; }
  else if (expr == "angry") { avatar.setExpression(Expression::Angry); baseExpr = Expression::Angry; }
  else if (expr == "sleepy") { avatar.setExpression(Expression::Sleepy); baseExpr = Expression::Sleepy; }
  else if (expr == "doubt") { avatar.setExpression(Expression::Doubt); baseExpr = Expression::Doubt; }
  else if (expr == "love" || expr == "eyeroll") {
    avatar.setExpression(Expression::Neutral);
    baseExpr = Expression::Neutral;
    g_customExpr = expr;
  }
  else { valid = false; }

  if (!valid) {
    server.send(400, "application/json", "{\"error\":\"unknown expression. options: neutral/happy/sad/angry/sleepy/doubt/love/eyeroll\"}");
    return;
  }
  currentExprName = expr;
  touchExprActive = false;
  if (expr != "neutral") {
    httpExprActive = true;
    httpExprTime = millis();
  } else {
    httpExprActive = false;
  }
  server.send(200, "application/json", "{\"ok\":true,\"expr\":\"" + expr + "\"}");
}

void handleServo() {
  int yaw = server.hasArg("yaw") ? server.arg("yaw").toInt() : 0;
  int pitch = server.hasArg("pitch") ? server.arg("pitch").toInt() : 450;
  int speed = server.hasArg("speed") ? server.arg("speed").toInt() : 500;
  yaw = constrain(yaw, -1280, 1280);
  pitch = constrain(pitch, 0, 900);
  speed = constrain(speed, 0, 1000);
  M5StackChan.Motion.move(yaw, pitch, speed);
  server.send(200, "application/json",
    "{\"ok\":true,\"yaw\":" + String(yaw) +
    ",\"pitch\":" + String(pitch) +
    ",\"speed\":" + String(speed) + "}");
}

void handleCamera() {
  if (!cameraReady) {
    server.send(503, "application/json", "{\"error\":\"camera not ready\"}");
    return;
  }
  camera_fb_t* old = esp_camera_fb_get();
  if (old) esp_camera_fb_return(old);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "application/json", "{\"error\":\"capture failed\"}");
    return;
  }
  uint8_t* jpg_buf = NULL;
  size_t jpg_len = 0;
  bool converted = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);
  if (!converted || !jpg_buf) {
    server.send(500, "application/json", "{\"error\":\"jpeg conversion failed\"}");
    return;
  }
  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char*)jpg_buf, jpg_len);
  free(jpg_buf);
}

void handleStatus() {
  float voltage = M5StackChan.getBatteryVoltage();
  float current = M5StackChan.getBatteryCurrent();
  auto angles = M5StackChan.Motion.getCurrentAngles();
  String json = "{";
  json += "\"battery_v\":" + String(voltage, 2);
  json += ",\"battery_ma\":" + String(current, 2);
  json += ",\"yaw\":" + String(angles.x);
  json += ",\"pitch\":" + String(angles.y);
  json += ",\"camera\":" + String(cameraReady ? "true" : "false");
  json += ",\"camera_err\":\"" + cameraError + "\"";
  json += ",\"expr\":\"" + currentExprName + "\"";
  json += ",\"uptime_s\":" + String(millis() / 1000);
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"rssi\":" + String(WiFi.RSSI());
  json += "}";
  server.send(200, "application/json", json);
}

void handleHome() {
  M5StackChan.Motion.goHome();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleTouch() {
  auto intensities = M5StackChan.TouchSensor.getIntensities();
  String json = "{";
  json += "\"front\":" + String(intensities[0]);
  json += ",\"middle\":" + String(intensities[1]);
  json += ",\"back\":" + String(intensities[2]);
  json += ",\"pressed\":" + String(M5StackChan.TouchSensor.isPressed() ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// ============ Audio Handlers ============

static const uint32_t MAX_RECORD_SAMPLES = RECORD_SAMPLE_RATE * 15;
static const uint32_t MAX_RECORD_BYTES = MAX_RECORD_SAMPLES * sizeof(int16_t);
static const uint32_t MAX_WAV_BYTES = 44 + MAX_RECORD_BYTES;
static int16_t* audioBuffer = NULL;
static uint8_t* wavBuffer = NULL;

void initAudioBuffers() {
  audioBuffer = (int16_t*)heap_caps_malloc(MAX_RECORD_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  wavBuffer = (uint8_t*)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("Audio buffers: audio=%p wav=%p\n", audioBuffer, wavBuffer);
}

unsigned long lastRecordEnd = 0;
const unsigned long RECORD_COOLDOWN = 500;

float chunkRMS(int16_t* buf, int len) {
  int64_t sum = 0;
  for (int i = 0; i < len; i++) sum += (int64_t)buf[i] * buf[i];
  return sqrt((float)sum / len);
}

void handleRecord() {
  if (millis() - lastRecordEnd < RECORD_COOLDOWN) {
    server.send(429, "application/json", "{\"error\":\"cooldown\"}");
    return;
  }
  if (!audioBuffer || !wavBuffer) {
    server.send(500, "application/json", "{\"error\":\"audio buffers not initialized\"}");
    return;
  }

  int seconds = server.hasArg("seconds") ? server.arg("seconds").toInt() : 3;
  seconds = constrain(seconds, 1, 15);
  bool useVad = server.hasArg("vad") && server.arg("vad") == "1";
  bool showLed = !server.hasArg("led") || server.arg("led") != "0";

  uint32_t maxSamples = RECORD_SAMPLE_RATE * seconds;

  if (showLed) M5StackChan.showRgbColor(0, 60, 0);
  startMic();
  delay(100);

  const int chunkSize = 1600;
  uint32_t recorded = 0;
  int silenceChunks = 0;
  bool speechDetected = false;
  const int SILENCE_THRESHOLD = 150;
  const int SPEECH_THRESHOLD = 200;
  const int SILENCE_NEEDED = 30;

  while (recorded < maxSamples) {
    int n = min((uint32_t)chunkSize, maxSamples - recorded);
    M5.Mic.record(audioBuffer + recorded, n, RECORD_SAMPLE_RATE);
    recorded += n;

    if (useVad) {
      float rms = chunkRMS(audioBuffer + recorded - n, n);
      if (!speechDetected) {
        if (rms > SPEECH_THRESHOLD) speechDetected = true;
      } else {
        if (rms < SILENCE_THRESHOLD) {
          silenceChunks++;
          if (silenceChunks >= SILENCE_NEEDED) break;
        } else {
          silenceChunks = 0;
        }
      }
    }
  }

  M5.Mic.end();
  micActive = false;
  if (showLed) M5StackChan.showRgbColor(0, 0, 0);

  uint32_t dataLen = recorded * sizeof(int16_t);
  uint32_t wavLen = 44 + dataLen;
  writeWavHeader(wavBuffer, dataLen);
  memcpy(wavBuffer + 44, audioBuffer, dataLen);

  server.sendHeader("Content-Disposition", "inline; filename=recording.wav");
  server.send_P(200, "audio/wav", (const char*)wavBuffer, wavLen);
  lastRecordEnd = millis();
}

void handlePlay() {
  if (!server.hasArg("url")) {
    server.send(400, "application/json", "{\"error\":\"url parameter required. Usage: /play?url=http://host/file.wav\"}");
    return;
  }

  String url = server.arg("url");

  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();
  if (httpCode != 200) {
    http.end();
    server.send(502, "application/json",
      "{\"error\":\"fetch failed\",\"url\":\"" + url + "\",\"code\":" + String(httpCode) + "}");
    return;
  }

  size_t len = http.getSize();
  if (len < 44 || len > 2000000) {
    http.end();
    server.send(400, "application/json",
      "{\"error\":\"bad size\",\"size\":" + String(len) + ",\"max\":2000000}");
    return;
  }

  if (g_playBuf) { free(g_playBuf); g_playBuf = nullptr; }

  uint8_t* wavBuf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wavBuf) { wavBuf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT); }
  if (!wavBuf) {
    http.end();
    server.send(500, "application/json", "{\"error\":\"malloc failed\",\"bytes\":" + String(len) + "}");
    return;
  }
  g_playBuf = wavBuf;

  // Streaming playback: parse WAV header, start speaker, feed PCM chunks
  // as they download. First sound comes out immediately, not after full download.
  WiFiClient* stream = http.getStreamPtr();
  size_t read = 0;
  unsigned long lastProgress = millis();

  bool started = false;
  size_t dataOffset = 0;
  size_t playEnd = 0;
  uint32_t sampleRate = 16000;
  size_t playedUpTo = 0;
  const size_t CHUNK_BYTES = 6400;

  while (read < len && (millis() - lastProgress < 10000)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = min(avail, len - read);
      size_t got = stream->readBytes(wavBuf + read, toRead);
      read += got;
      if (got > 0) lastProgress = millis();
    } else {
      delay(1);
    }

    if (!started && read >= 44) {
      if (wavBuf[0]!='R' || wavBuf[1]!='I' || wavBuf[2]!='F' || wavBuf[3]!='F') {
        break;
      }
      size_t pos = 12;
      uint32_t dataSize = 0;
      while (pos + 8 <= read) {
        uint32_t chunkSize; memcpy(&chunkSize, wavBuf+pos+4, 4);
        if (wavBuf[pos]=='d'&&wavBuf[pos+1]=='a'&&wavBuf[pos+2]=='t'&&wavBuf[pos+3]=='a') {
          dataOffset = pos + 8; dataSize = chunkSize; break;
        }
        pos += 8 + chunkSize;
      }
      if (dataOffset > 0 && dataOffset < len) {
        memcpy(&sampleRate, wavBuf+24, 4);
        size_t audioLen = len - dataOffset;
        if (dataSize > 0 && audioLen > dataSize) audioLen = dataSize;
        playEnd = dataOffset + audioLen;
        startSpeaker();
        started = true;
        playedUpTo = dataOffset;
        g_lipData = (int16_t*)(wavBuf + dataOffset);
        g_lipSamples = audioLen / sizeof(int16_t);
        g_lipRate = sampleRate;
        g_lipSyncActive = false;
      }
    }

    if (started) {
      size_t cap = (read < playEnd) ? read : playEnd;
      size_t pending = (cap - playedUpTo) & ~((size_t)1);
      bool last = (cap >= playEnd);
      if (pending >= CHUNK_BYTES || (last && pending > 0)) {
        unsigned long w = millis();
        while (M5.Speaker.isPlaying(0) >= 2 && millis() - w < 3000) { updateLipSync(); delay(1); }
        M5.Speaker.playRaw((const int16_t*)(wavBuf + playedUpTo),
                           pending / sizeof(int16_t), sampleRate, false, 1, 0, false);
        playedUpTo += pending;
        if (!g_lipSyncActive) {
          g_lipStart = millis();
          g_lipLastUpdate = 0;
          g_lipSyncActive = true;
        }
      }
    }

    updateLipSync();
  }
  http.end();

  if (!started || playEnd == 0) {
    free(wavBuf); g_playBuf = nullptr;
    g_lipSyncActive = false;
    server.send(400, "application/json", "{\"error\":\"not a WAV or no data chunk\"}");
    return;
  }

  server.send(200, "application/json",
    "{\"ok\":true,\"streamed\":true,\"bytes\":" + String(len) +
    ",\"rate\":" + String(sampleRate) + "}");
}

// Stream mic PCM to host via TCP (for real-time STT)
void handleStream() {
  int port = server.hasArg("port") ? server.arg("port").toInt() : 7073;
  WiFiClient client;
  if (!client.connect(CALLBACK_HOST, port)) {
    server.send(502, "application/json", "{\"error\":\"stream connect failed\"}");
    return;
  }
  client.setNoDelay(true);

  bool showLed = !server.hasArg("led") || server.arg("led") != "0";
  if (showLed) M5StackChan.showRgbColor(0, 60, 0);
  startMic();
  delay(100);

  static int16_t chunk[1600];
  size_t sentBytes = 0;
  unsigned long t0 = millis();

  while (client.connected() && millis() - t0 < 120000) {
    M5.Mic.record(chunk, 1600, RECORD_SAMPLE_RATE);
    size_t off = 0;
    bool dead = false;
    while (off < sizeof(chunk)) {
      size_t n = client.write(((const uint8_t*)chunk) + off, sizeof(chunk) - off);
      if (n == 0) { dead = true; break; }
      off += n;
    }
    sentBytes += off;
    if (dead) break;
  }
  client.stop();
  M5.Mic.end();
  micActive = false;
  if (showLed) M5StackChan.showRgbColor(0, 0, 0);

  server.send(200, "application/json",
    "{\"ok\":true,\"bytes\":" + String(sentBytes) +
    ",\"secs\":" + String(sentBytes / 32000.0, 1) + "}");
}

void handleSpeech() {
  String text = server.arg("text");
  g_speechDurMs = server.hasArg("dur") ? (unsigned long)server.arg("dur").toInt() : 0;
  g_speechStart = millis();
  buildSubtitle(text);
  server.send(200, "application/json",
    "{\"ok\":true,\"text\":\"" + text + "\"}");
}

void handleVolume() {
  const int sampleCount = 1600;
  int16_t* buf = (int16_t*)malloc(sampleCount * sizeof(int16_t));
  if (!buf) {
    server.send(500, "application/json", "{\"error\":\"malloc failed\"}");
    return;
  }

  startMic();
  M5.Mic.record(buf, sampleCount, RECORD_SAMPLE_RATE);

  int64_t sumSq = 0;
  int16_t peak = 0;
  for (int i = 0; i < sampleCount; i++) {
    int16_t s = buf[i];
    sumSq += (int64_t)s * s;
    if (abs(s) > peak) peak = abs(s);
  }
  free(buf);

  float rms = sqrt((float)sumSq / sampleCount);

  server.send(200, "application/json",
    "{\"rms\":" + String(rms, 1) +
    ",\"peak\":" + String(peak) +
    ",\"threshold_suggestion\":" + String((int)(rms * 2)) + "}");
}

// ============ Lip Sync ============

void updateLipSync() {
  if (!g_lipSyncActive) return;
  if (!M5.Speaker.isPlaying()) {
    g_lipSyncActive = false;
    avatar.setMouthOpenRatio(0.0f);
    return;
  }
  if (millis() - g_lipLastUpdate < 40) return;
  g_lipLastUpdate = millis();
  uint32_t pos = (uint32_t)((millis() - g_lipStart) / 1000.0f * g_lipRate);
  if (pos < g_lipSamples) {
    uint32_t win = g_lipRate / 50;
    if (pos + win > g_lipSamples) win = g_lipSamples - pos;
    int64_t sum = 0;
    for (uint32_t i = 0; i < win; i++) {
      int32_t s = g_lipData[pos + i];
      sum += (int64_t)s * s;
    }
    float rms = sqrtf((float)sum / win);
    float ratio = rms / LIP_RMS_SCALE;
    if (ratio > 1.0f) ratio = 1.0f;
    avatar.setMouthOpenRatio(ratio);
  }
}

// ============ Setup & Loop ============

void setup() {
  Serial.begin(115200);
  M5StackChan.begin();

  M5StackChan.Display().setTextSize(2);
  M5StackChan.Display().setTextColor(TFT_WHITE, TFT_BLACK);
  M5StackChan.Display().clear();
  M5StackChan.Display().setCursor(10, 10);
  M5StackChan.Display().println("Starting up...");

  // WiFi with static IP
  IPAddress staticIP(STATIC_IP);
  IPAddress gateway(GATEWAY_IP);
  IPAddress subnet(SUBNET_MASK);
  IPAddress dnsIP(DNS_IP);
  WiFi.config(staticIP, gateway, subnet, dnsIP);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  M5StackChan.Display().print("WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    M5StackChan.Display().print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    M5StackChan.Display().println(" OK");
    M5StackChan.Display().print("IP: ");
    M5StackChan.Display().println(WiFi.localIP().toString());
  } else {
    M5StackChan.Display().println(" FAIL");
  }

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  initAudioBuffers();
  M5StackChan.Display().println(audioBuffer ? "Audio OK" : "Audio FAIL");

  M5StackChan.Display().print("Camera...");
  cameraReady = initCamera();
  M5StackChan.Display().println(cameraReady ? " OK" : " FAIL");

  imuReady = M5.Imu.isEnabled();
  M5StackChan.Display().println(imuReady ? "IMU OK" : "IMU N/A");

  server.on("/", handleRoot);
  server.on("/face", handleFace);
  server.on("/servo", handleServo);
  server.on("/camera", handleCamera);
  server.on("/status", handleStatus);
  server.on("/home", handleHome);
  server.on("/touch", handleTouch);
  server.on("/record", handleRecord);
  server.on("/stream", handleStream);
  server.on("/play", handlePlay);
  server.on("/volume", handleVolume);
  server.on("/speech", handleSpeech);
  server.begin();

  M5StackChan.Display().println("Server started!");
  delay(1500);

  auto *mouth    = new CustomMouth();
  auto *eyeR     = new CustomEye(false);
  auto *eyeL     = new CustomEye(true);
  auto *browR    = new CustomBrow(false);
  auto *browL    = new CustomBrow(true);
  auto *mouthPos = new BoundingRect(148, 163);
  auto *eyeRPos  = new BoundingRect(93, 230);
  auto *eyeLPos  = new BoundingRect(96, 103);
  auto *browRPos = new BoundingRect(67, 230);
  auto *browLPos = new BoundingRect(70, 103);
  auto *face = new Face(mouth, mouthPos, eyeR, eyeRPos, eyeL, eyeLPos,
                        browR, browRPos, browL, browLPos);
  avatar.init();
  avatar.setFace(face);
  avatar.setExpression(Expression::Neutral);

  M5StackChan.Motion.goHome(300);
}

void loop() {
  M5StackChan.update();
  server.handleClient();

  // WiFi auto-reconnect
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }

  if (millis() < 5000) { delay(10); return; }

  // Shake detection
  if (imuReady && millis() - lastShakeCheck > 50) {
    lastShakeCheck = millis();
    float ax = 0, ay = 0, az = 0;
    M5.Imu.getAccelData(&ax, &ay, &az);
    float mag = sqrtf(ax*ax + ay*ay + az*az);
    float delta = fabsf(mag - prevAccelMag);
    prevAccelMag = mag;

    if (delta > SHAKE_THRESHOLD) {
      shakeCount++;
    } else if (shakeCount > 0) {
      shakeCount--;
    }

    if (shakeCount >= 3 && millis() - lastShakeReaction > SHAKE_COOLDOWN) {
      lastShakeReaction = millis();
      shakeCount = 0;
      avatar.setExpression(Expression::Doubt);
      touchExprActive = true;
      touchExprTime = millis();
      M5StackChan.showRgbColor(255, 255, 0);
      M5StackChan.Motion.move(400, 450, 500);
      delay(200);
      M5StackChan.showRgbColor(0, 255, 255);
      M5StackChan.Motion.move(-400, 450, 500);
      delay(200);
      M5StackChan.showRgbColor(255, 0, 255);
      M5StackChan.Motion.move(200, 450, 500);
      delay(200);
      M5StackChan.showRgbColor(0, 0, 0);
      M5StackChan.Motion.goHome(300);
      notifyCallback("shake", TOUCH_PORT);
    }
  }

  // Touch reactions
  bool swiped = false;
  if (M5StackChan.TouchSensor.wasSwipedForward()) {
    swiped = true;
    avatar.setExpression(Expression::Happy);
    touchExprActive = true;
    touchExprTime = millis();
    M5StackChan.showRgbColor(255, 50, 50);
    M5StackChan.Motion.movePitch(600, 400);
    delay(300);
    M5StackChan.Motion.movePitch(300, 400);
    delay(300);
    M5StackChan.showRgbColor(0, 0, 0);
    notifyCallback("pet", TOUCH_PORT);
  }
  if (M5StackChan.TouchSensor.wasSwipedBackward()) {
    swiped = true;
    avatar.setExpression(Expression::Doubt);
    touchExprActive = true;
    touchExprTime = millis();
    notifyCallback("pet_reverse", TOUCH_PORT);
  }
  if (!swiped && M5StackChan.TouchSensor.wasPressed()) {
    int hits = 0;
    for (int i = 0; i < 5; i++) {
      delay(8);
      M5StackChan.update();
      if (M5StackChan.TouchSensor.isPressed()) hits++;
    }
    if (hits >= 3) {
      unsigned long now = millis();
      if (lastValidTap > 0 && (now - lastValidTap) < DOUBLE_TAP_WINDOW) {
        lastValidTap = 0;
        avatar.setExpression(Expression::Happy);
        touchExprActive = true;
        touchExprTime = millis();
        M5StackChan.showRgbColor(255, 100, 100);
        delay(50);
        M5StackChan.showRgbColor(0, 0, 0);
        notifyCallback("tap", TOUCH_PORT);
      } else {
        lastValidTap = now;
        M5StackChan.showRgbColor(50, 50, 100);
        delay(50);
        M5StackChan.showRgbColor(0, 0, 0);
      }
    }
  }

  // Screen touch
  static bool screenWasTouched = false;
  M5.update();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed() && !screenWasTouched) {
    screenWasTouched = true;
    avatar.setExpression(Expression::Happy);
    touchExprActive = true;
    touchExprTime = millis();
    notifyCallback("screen_tap", VOICE_PORT);
  }
  if (t.wasReleased()) {
    screenWasTouched = false;
  }

  updateLipSync();

  // Reset touch expression after 3 seconds
  if (touchExprActive && (millis() - touchExprTime > 3000)) {
    touchExprActive = false;
    avatar.setExpression(baseExpr);
    avatar.setEyeOpenRatio(1.0);
    avatar.setLeftGaze(0, 0);
    avatar.setRightGaze(0, 0);
    avatar.setIsAutoBlink(true);
  }

  // Reset HTTP expression after 30 seconds
  if (httpExprActive && (millis() - httpExprTime > 30000)) {
    httpExprActive = false;
    baseExpr = Expression::Neutral;
    currentExprName = "neutral";
    g_customExpr = "";
    avatar.setExpression(Expression::Neutral);
  }

  delay(10);
}
