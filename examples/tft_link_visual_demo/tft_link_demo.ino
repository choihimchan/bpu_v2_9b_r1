#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// =====================
// TFT PIN MAP (Your wiring)
// =====================
#define TFT_CS    27
#define TFT_DC    16
#define TFT_RST   33
#define TFT_MOSI  23
#define TFT_SCLK  18

// =====================
// BUTTON (MX Switch)
// =====================
#define KEY_PIN   25   // one side to GPIO25, other side to GND

// =====================
// DISPLAY SIZE
// =====================
#define TFT_W 240
#define TFT_H 320

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// =====================
// COLORS
// =====================
static const uint16_t COL_DARKGREY = 0x7BEF; // safe grey
static const uint16_t COL_BG       = ST77XX_BLACK;

// =====================
// UI / Demo State
// =====================
enum LinkState {
  LINK_UP_STATE = 0,
  LINK_DOWN_STATE,
  RESYNC_STATE
};

static LinkState linkState = LINK_UP_STATE;

// Counters to look like your BPU logs
static uint32_t seq     = 0;
static uint32_t ack     = 0;
static uint32_t retx    = 0;
static uint32_t rxBytes = 0;

static uint32_t frames = 0;
static uint32_t lastFrameMs = 0;
static float fps = 0.0f;

// For demo timing
static uint32_t stateStartMs = 0;
static uint8_t  syncAckCount = 0;

// =====================
// Simple PRNG for "noise"
// =====================
static uint16_t noiseSeed = 0x1234;
static inline uint16_t xorshift16() {
  noiseSeed ^= noiseSeed << 7;
  noiseSeed ^= noiseSeed >> 9;
  noiseSeed ^= noiseSeed << 8;
  return noiseSeed;
}

// =====================
// Button Debounce
// =====================
bool buttonPressed() {
  static uint32_t lastMs = 0;
  static bool prev = true; // pullup => idle HIGH(true)
  bool cur = (digitalRead(KEY_PIN) == HIGH);

  // detect falling edge (HIGH->LOW)
  bool falling = (prev == true && cur == false);
  prev = cur;

  if (falling && (millis() - lastMs > 250)) {
    lastMs = millis();
    return true;
  }
  return false;
}

// =====================
// UI Drawing
// =====================
void drawHeaderNoise() {
  // Top "TV snow" band
  for (int y = 0; y < 58; y += 2) {
    for (int x = 0; x < TFT_W; x += 4) {
      uint16_t r = xorshift16();
      uint8_t v = (uint8_t)(r & 0xFF);
      uint16_t c = tft.color565(v, v, v);
      tft.fillRect(x, y, 4, 2, c);
    }
  }

  // Title bar
  tft.fillRect(0, 58, TFT_W, 18, COL_DARKGREY);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(8, 60);
  tft.print(" BPU2 DEMO");
}

void drawStaticFrame() {
  tft.fillScreen(COL_BG);
  drawHeaderNoise();

  // Separator
  tft.drawLine(0, 78, TFT_W - 1, 78, COL_DARKGREY);

  tft.setTextSize(1);
  tft.setTextColor(COL_DARKGREY);
  tft.setCursor(8, 88);
  tft.print("BTN(GPIO25) = INJECT LINK DROP (3s) -> RESYNC");

  // bottom bar outline
  tft.drawRect(8, 290, TFT_W - 16, 18, COL_DARKGREY);
}

void drawLinkStatus() {
  // clear main area
  tft.fillRect(0, 105, TFT_W, 175, COL_BG);

  // big link text
  tft.setTextSize(3);
  tft.setCursor(10, 112);

  if (linkState == LINK_UP_STATE) {
    tft.setTextColor(ST77XX_GREEN);
    tft.print("LINK UP");
  } else if (linkState == LINK_DOWN_STATE) {
    tft.setTextColor(ST77XX_RED);
    tft.print("LINK DOWN");
  } else {
    tft.setTextColor(ST77XX_YELLOW);
    tft.print("RESYNC");
  }

  // detail lines
  tft.setTextSize(2);

  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(10, 150);  tft.print("SEQ : ");  tft.print(seq);
  tft.setCursor(10, 172);  tft.print("ACK : ");  tft.print(ack);

  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(10, 194);  tft.print("RETX: ");  tft.print(retx);

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 216);  tft.print("RX  : ");  tft.print(rxBytes); tft.print(" B");

  tft.setTextColor(COL_DARKGREY);
  tft.setCursor(10, 240);  tft.print("FPS : ");  tft.print(fps, 1);

  // status line like logs
  tft.setTextSize(1);
  tft.setTextColor(COL_DARKGREY);
  tft.setCursor(10, 262);

  if (linkState == LINK_UP_STATE) {
    tft.print("State: UP (stream ok)");
  } else if (linkState == LINK_DOWN_STATE) {
    tft.print("State: DOWN (waiting ACK / idle timeout)");
  } else {
    tft.print("State: RESYNC (SYNC_ACK x2)");
  }
}

void drawProgressBar() {
  uint8_t p = (uint8_t)(frames % 101);
  int barX = 10, barY = 292, barW = TFT_W - 20, barH = 14;

  tft.fillRect(barX, barY, barW, barH, 0x1082);
  int fillW = (barW * p) / 100;

  uint16_t col = (linkState == LINK_UP_STATE) ? ST77XX_GREEN :
                 (linkState == LINK_DOWN_STATE) ? ST77XX_RED : ST77XX_YELLOW;

  tft.fillRect(barX, barY, fillW, barH, col);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 274);
  tft.print("BTN to inject drop  |  Frame ");
  tft.print(frames);
}

// =====================
// Demo Logic
// =====================
void enterDownState() {
  linkState = LINK_DOWN_STATE;
  stateStartMs = millis();
  syncAckCount = 0;

  // like your log: drop triggers retries
  retx += 1;
}

void enterResyncState() {
  linkState = RESYNC_STATE;
  stateStartMs = millis();
  syncAckCount = 0;
}

void enterUpState() {
  linkState = LINK_UP_STATE;
  stateStartMs = millis();
  syncAckCount = 0;

  // resync succeeded -> ack catch up
  ack = seq;
}

void updateDemoStateMachine() {
  uint32_t now = millis();

  if (linkState == LINK_UP_STATE) {
    // normal stream
    seq++;
    ack = seq;                 // clean link: ack tracks
    rxBytes += 320 + (seq % 50);

  } else if (linkState == LINK_DOWN_STATE) {
    // during down: seq may still increase but ack stalls
    seq++;
    rxBytes += 60;              // very little rx during down

    // inject "more retries" while down
    if ((seq % 2) == 0) retx++;

    // hold down for 3s then go resync
    if (now - stateStartMs >= 3000) {
      enterResyncState();
    }

  } else { // RESYNC_STATE
    // show SYNC_ACK twice over ~500ms~800ms
    // After 2 acks -> up
    if (syncAckCount < 2) {
      // every 250ms, count one SYNC_ACK
      if (now - stateStartMs >= (uint32_t)(250 * (syncAckCount + 1))) {
        syncAckCount++;
        rxBytes += 120; // got a sync ack packet
      }
    } else {
      // after short moment, link up
      if (now - stateStartMs >= 800) {
        enterUpState();
      }
    }
  }
}

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(200);

  // Button
  pinMode(KEY_PIN, INPUT_PULLUP);

  // SPI on custom pins
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  // TFT init
  tft.init(TFT_W, TFT_H);
  tft.setRotation(0);

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(20, 140);
  tft.print("ST7789 READY");
  delay(400);

  drawStaticFrame();
  enterUpState();
}

void loop() {
  // FPS calc
  uint32_t now = millis();
  if (lastFrameMs != 0) {
    uint32_t dt = now - lastFrameMs;
    if (dt > 0) fps = 1000.0f / (float)dt;
  }
  lastFrameMs = now;
  frames++;

  // Button triggers: inject down (3s)
  if (buttonPressed()) {
    // always inject a drop no matter current state
    enterDownState();
  }

  // Demo state update (seq/ack/retx/rxBytes)
  updateDemoStateMachine();

  // Update UI
  // refresh noise lightly
  static uint32_t lastNoise = 0;
  if (now - lastNoise > 180) {
    lastNoise = now;
    for (int y = 0; y < 58; y += 3) {
      for (int x = 0; x < TFT_W; x += 6) {
        uint16_t r = xorshift16();
        uint8_t v = (uint8_t)(r & 0xFF);
        uint16_t c = tft.color565(v, v, v);
        tft.fillRect(x, y, 6, 3, c);
      }
    }
  }

  drawLinkStatus();
  drawProgressBar();

  delay(33); // ~30fps
}
