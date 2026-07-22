// ============================================================
// ESP_Checkpoint_v2.ino — TUG Test: Checkpoint & RGB Controller
// ============================================================
// บอร์ดนี้ติดตั้งที่จุดหมุนตัวระยะ 3 เมตร
//   • ตรวจจับว่าผู้ทดสอบเดินมาถึงแล้ว (ultrasonic) แล้วส่งสัญญาณกลับไป Chair
//   • แสดงผลความเสี่ยงด้วยไฟ RGB หลังได้รับ FINISH
//   • รายงานสถานะตัวเองขึ้น Firestore ให้เว็บเห็น (device_status/checkpoint)
//   • รับคำสั่งรีเซ็ตจากเว็บได้เอง (device_commands/checkpoint)
//
// Library ที่ต้องติดตั้ง (Arduino Library Manager):
//   → "Firebase ESP Client" by Mobizt
//
// ⚠️  struct_message และ TUG_*_RISK_MAX ต้องตรงกับ ESP_Chair เสมอ
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "secrets.h"  // Copy from secrets.h.example; never commit secrets.h

#define FW_VERSION "checkpoint-2.0.0"

// ============================================================
// ⚙️  USER CONFIGURATION — แก้ไขค่าเหล่านี้ก่อนอัปโหลด
//     ใช้ค่าเดียวกับที่ตั้งไว้ใน ESP_Chair_v2.ino
// ============================================================

// WiFi, Firebase and ESP-NOW keys are defined in the ignored secrets.h file.

// ============================================================

// ---------- Pin Configuration ----------
#define TRIG_PIN    18
#define ECHO_PIN    19

#define RED_PIN     25
#define GREEN_PIN   26
#define BLUE_PIN    27

// ---------- Distance Thresholds (cm) ----------
#define DIST_DETECT        30.0   // ตรวจพบคนภายใน 30 ซม.
#define DIST_MAX_VALID    400.0   // ระยะสูงสุดที่เป็นไปได้
#define DIST_TIMEOUT      999.0   // ค่าที่คืนเมื่อไม่มี echo

// ---------- Timing Configuration (ms) ----------
#define DEBOUNCE_DETECT_MS 300
#define SERIAL_INTERVAL_MS 500
#define WIFI_TIMEOUT_MS    10000

// [แก้ข้อ 10] เพดานเวลาของการเฝ้ารอ — ถ้า Chair รีบูตหรือแพ็กเก็ต ABORT หาย
// บอร์ดนี้จะไม่ค้างอยู่ในสถานะ DETECTING ตลอดไป
#define DETECT_TIMEOUT_MS     130000  // ยาวกว่า MAX_TEST_DURATION_MS ของ Chair เล็กน้อย
#define RESULT_DISPLAY_MS     20000   // แสดงไฟผลลัพธ์นานเท่าไรก่อนกลับเป็นไฟ "พร้อม"

#define HEARTBEAT_INTERVAL_MS    15000
#define COMMAND_POLL_INTERVAL_MS 6000

// ---------- Median / Valid-only Filter ----------
#define MEDIAN_SAMPLES      5
#define VALID_WINDOW        5

// ============================================================
// [แก้ข้อ 1] เกณฑ์ความเสี่ยง TUG — ต้องตรงกับ ESP_Chair และเว็บ
//   ≤ 11 วิ = LOW | > 11–30 วิ = MODERATE | > 30 วิ = HIGH
// ค่าเดิมคือ 20.0 ซึ่งไม่ตรงเอกสารโครงการหัวข้อ 6.5.1
// ============================================================
#define TUG_LOW_RISK_MAX   11.0
#define TUG_MOD_RISK_MAX   30.0

// ---------- RGB LED (Common Anode: LOW = ON, HIGH = OFF) ----------
#define LED_ON   LOW
#define LED_OFF  HIGH

// ---------- ESP-NOW Peer (Chair ESP32) ----------
// ดู MAC ของอีกบอร์ดได้จาก Serial ตอนบูต (บอร์ดจะพิมพ์ MAC ของตัวเองออกมา)
uint8_t chairMAC[] = {0x88, 0x57, 0x21, 0xB6, 0x70, 0x84};

// ---------- Communication Struct (ต้องเหมือน ESP_Chair เป๊ะ) ----------
typedef struct struct_message {
  char     command[15];
  float    timeSec;
  uint32_t runId;
} struct_message;

// ---------- State Machine ----------
enum CheckpointState {
  CP_IDLE,       // รอคำสั่ง START — ไฟน้ำเงิน = พร้อมใช้งาน
  CP_DETECTING,  // ได้รับ START แล้ว กำลังเฝ้ารอผู้ทดสอบเดินมาถึง
  CP_RESULT      // ได้รับ FINISH แล้ว กำลังแสดงไฟผลลัพธ์
};

// ---------- Firebase Objects ----------
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig firebaseConfig;

// ---------- Global Variables ----------
struct_message      msgData;   // buffer สำหรับ "ส่ง" เท่านั้น
esp_now_peer_info_t peerInfo;

CheckpointState currentState = CP_IDLE;

unsigned long debounceStart   = 0;
unsigned long lastPrintTime   = 0;
unsigned long detectStart     = 0;   // เวลาที่เข้าสู่ CP_DETECTING
unsigned long resultStart     = 0;   // เวลาที่เข้าสู่ CP_RESULT
unsigned long lastHeartbeat   = 0;
unsigned long lastCommandPoll = 0;

bool debounceActive = false;
bool wifiConnected  = false;
bool firebaseReady  = false;

uint32_t currentRunId = 0;   // runId ของรอบที่กำลังทำอยู่ (ได้มาจาก START)
float    lastResultSec = 0;  // เวลารวมของรอบล่าสุด ไว้โชว์ในสถานะ

// ---------- Valid-only distance filter ----------
float validWindow[VALID_WINDOW];
int   validHead   = 0;
int   validCount  = 0;
float lastValidDistance = DIST_TIMEOUT;
bool  distanceHeld = false;

// ---------- Chair link status ----------
volatile unsigned long lastChairAck = 0;

// ==========================================================
// Utilities
// ==========================================================
const char* getStateName(CheckpointState s) {
  switch (s) {
    case CP_IDLE:      return "IDLE";
    case CP_DETECTING: return "DETECTING";
    case CP_RESULT:    return "RESULT";
    default:           return "UNKNOWN";
  }
}

const char* riskLevelOf(float totalSec) {
  if (totalSec <= TUG_LOW_RISK_MAX) return "LOW";
  if (totalSec <= TUG_MOD_RISK_MAX) return "MODERATE";
  return "HIGH";
}

uint32_t getEpoch() {
  time_t t = time(nullptr);
  return (t < 100000) ? 0 : (uint32_t)t;
}

// ==========================================================
// Ultrasonic
// ==========================================================
float readDistanceRaw() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return DIST_TIMEOUT;
  return duration * 0.034 / 2.0;
}

// [แก้ข้อ 12] ตัวกรองแบบเอาเฉพาะค่า valid — เดิมบอร์ดนี้ยังใช้ median แบบเก่า
// ที่เอา 999.0 (timeout) มาคิดเป็นระยะด้วย ทำให้ค่า median เด้งสูงผิดปกติ
// และ "พลาดการตรวจจับ" ตอนผู้ทดสอบเดินผ่านจริง ๆ
//   ① ยิงเซ็นเซอร์เป็นชุด เก็บเฉพาะค่าที่ valid (0 < d < DIST_MAX_VALID)
//   ② หา median จาก ring buffer ของค่า valid ล่าสุด
//   ③ ถ้ารอบนี้ไม่มีค่า valid เลย → คืนค่า valid ล่าสุด (hold last)
float getDistance() {
  int gotValid = 0;

  for (int i = 0; i < MEDIAN_SAMPLES; i++) {
    float raw = readDistanceRaw();

    if (raw > 0.0 && raw < DIST_MAX_VALID) {
      validWindow[validHead] = raw;
      validHead = (validHead + 1) % VALID_WINDOW;
      if (validCount < VALID_WINDOW) validCount++;
      gotValid++;
    }

    if (i < MEDIAN_SAMPLES - 1) delayMicroseconds(500);
  }

  if (gotValid == 0) {
    distanceHeld = true;
    return lastValidDistance;
  }
  distanceHeld = false;

  float tmp[VALID_WINDOW];
  for (int i = 0; i < validCount; i++) tmp[i] = validWindow[i];

  for (int i = 1; i < validCount; i++) {
    float key = tmp[i];
    int j = i - 1;
    while (j >= 0 && tmp[j] > key) {
      tmp[j + 1] = tmp[j];
      j--;
    }
    tmp[j + 1] = key;
  }

  lastValidDistance = tmp[validCount / 2];
  return lastValidDistance;
}

// ==========================================================
// RGB LED
// ==========================================================
void setRGB(bool r, bool g, bool b) {
  digitalWrite(RED_PIN,   r ? LED_ON : LED_OFF);
  digitalWrite(GREEN_PIN, g ? LED_ON : LED_OFF);
  digitalWrite(BLUE_PIN,  b ? LED_ON : LED_OFF);
}

void rgbOff()    { setRGB(false, false, false); }
void rgbGreen()  { setRGB(false, true,  false); }
void rgbYellow() { setRGB(true,  true,  false); }   // Red + Green = Yellow
void rgbRed()    { setRGB(true,  false, false); }
void rgbBlue()   { setRGB(false, false, true);  }   // ไฟ "พร้อมใช้งาน"

// [แก้ข้อ 8 บางส่วน] ไฟสถานะ "พร้อม/ไม่พร้อม" ตามที่เอกสารขั้นตอนที่ 2 กำหนด
//   น้ำเงิน = พร้อมใช้งาน (ออนไลน์ + เชื่อมต่อ Chair ได้)
//   ดับ     = กำลังทดสอบ (ไม่รบกวนสายตาผู้ทดสอบ)
//   เขียว/เหลือง/แดง = ผลการประเมิน
void showReadyLight() {
  rgbBlue();
}

// ==========================================================
// Cloud Firestore
// ==========================================================

// [แก้ข้อ 8] รายงานสถานะตัวเองขึ้น Firestore
// เดิมบอร์ดนี้ต่อ WiFi ไว้เฉย ๆ เพื่อ sync channel ของ ESP-NOW เท่านั้น
// เว็บจึงไม่มีทางรู้เลยว่า Checkpoint เปิดอยู่หรือเปล่า
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  uint32_t nowSec = getEpoch();
  if (nowSec == 0) return;

  bool chairOnline = (lastChairAck != 0) && (millis() - lastChairAck < 15000);

  FirebaseJson content;
  content.set("fields/online/booleanValue",       true);
  content.set("fields/last_seen/integerValue",    String((long)nowSec));
  content.set("fields/state/stringValue",         getStateName(currentState));
  content.set("fields/rssi/integerValue",         String((long)WiFi.RSSI()));
  content.set("fields/device/stringValue",        "checkpoint");
  content.set("fields/fw_version/stringValue",    FW_VERSION);
  content.set("fields/uptime_sec/integerValue",   String((long)(millis() / 1000)));
  content.set("fields/chair_online/booleanValue", chairOnline);

  if (!Firebase.Firestore.patchDocument(
        &fbdo, FIREBASE_PROJECT_ID, "(default)",
        "device_status/checkpoint", content.raw(),
        "online,last_seen,state,rssi,device,fw_version,uptime_sec,chair_online")) {
    Serial.println("  [Heartbeat] ❌ " + fbdo.errorReason());
  }
}

long readIntField(FirebaseJson& payload, const char* path) {
  FirebaseJsonData result;
  payload.get(result, path);
  // Firestore ส่ง integerValue มาเป็น string เสมอ
  return result.success ? result.to<String>().toInt() : 0;
}

// [แก้ข้อ 8] รับคำสั่งรีเซ็ตจากเว็บได้เอง ไม่ต้องเดินไปกดปุ่มที่บอร์ด
// หลักการ ack-before-restart เหมือนฝั่ง Chair:
// ต้องเขียน reset_handled_at สำเร็จก่อนรีบูต ไม่งั้นบูตขึ้นมาจะเจอคำสั่งเดิมค้าง
// แล้ววนรีบูตไม่รู้จบ
void checkResetCommand() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                      "device_commands/checkpoint", "")) {
    return;   // ยังไม่มี document — ไม่มีคำสั่งค้าง
  }

  FirebaseJson payload;
  payload.setJsonData(fbdo.payload());

  long resetReq = readIntField(payload, "fields/reset_requested_at/integerValue");
  long resetAck = readIntField(payload, "fields/reset_handled_at/integerValue");
  if (resetReq <= 0 || resetReq <= resetAck) return;

  Serial.println();
  Serial.println("========================================");
  Serial.println("  [Remote Reset] คำสั่งรีเซ็ตจากเว็บ — กำลังรีบูต...");
  Serial.println("========================================");

  FirebaseJson ack;
  ack.set("fields/reset_handled_at/integerValue", String(resetReq));

  if (!Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                        "device_commands/checkpoint", ack.raw(),
                                        "reset_handled_at")) {
    Serial.println("  [Remote Reset] ❌ ack ไม่สำเร็จ — ยกเลิกการรีบูต จะลองใหม่รอบหน้า");
    Serial.println("  [Remote Reset] Error : " + fbdo.errorReason());
    return;
  }

  delay(200);
  ESP.restart();
}

// ==========================================================
// ESP-NOW
// ==========================================================
void sendCommand(const char* cmd, float t) {
  strncpy(msgData.command, cmd, sizeof(msgData.command) - 1);
  msgData.command[sizeof(msgData.command) - 1] = '\0';
  msgData.timeSec = t;
  msgData.runId   = currentRunId;
  esp_now_send(chairMAC, (uint8_t *)&msgData, sizeof(msgData));
}

// ---------- ความเข้ากันได้ของ signature ระหว่าง core เวอร์ชันต่าง ๆ ----------
// ESP32 Arduino core 3.x เปลี่ยน argument ตัวแรกของ callback ทั้งสองตัว
// จาก "uint8_t* MAC" เป็น struct info โค้ดเดิมใช้ cast ทับซึ่งผิดชนิดจริง ๆ
// (ที่ยังไม่พังเพราะไม่เคยแตะ argument ตัวนั้น) ตรงนี้ประกาศให้ถูกต้อง
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define ESPNOW_SEND_CB_ARG  const esp_now_send_info_t *sendInfo
  #define ESPNOW_RECV_CB_ARG  const esp_now_recv_info_t *recvInfo
#else
  #define ESPNOW_SEND_CB_ARG  const uint8_t *sendInfo
  #define ESPNOW_RECV_CB_ARG  const uint8_t *recvInfo
#endif

void OnDataSent(ESPNOW_SEND_CB_ARG, esp_now_send_status_t status) {
  // ไม่พิมพ์ทุกครั้ง เพราะจะทำให้ log รก — เก็บไว้เป็นสัญญาณว่า Chair ยังออนไลน์
  if (status == ESP_NOW_SEND_SUCCESS) lastChairAck = millis();
}

void OnDataRecv(ESPNOW_RECV_CB_ARG, const uint8_t *incomingData, int len) {
  // ใช้ buffer แยกจาก msgData ที่ใช้ส่ง ไม่งั้นข้อมูลขารับจะไปทับข้อมูลขาส่ง
  struct_message in;
  if (len != sizeof(in)) return;
  memcpy(&in, incomingData, sizeof(in));
  in.command[sizeof(in.command) - 1] = '\0';

  // --- PING: ใช้เช็คลิงก์เท่านั้น ไม่ต้องทำอะไร (ACK ชั้น MAC ตอบให้เองแล้ว) ---
  if (strcmp(in.command, "PING") == 0) {
    lastChairAck = millis();
    return;
  }

  // --- START: เริ่มเฝ้ารอผู้ทดสอบ ---
  if (strcmp(in.command, "START") == 0) {
    currentRunId   = in.runId;    // จำ runId ของรอบนี้ไว้ตอบกลับ
    currentState   = CP_DETECTING;
    debounceActive = false;
    detectStart    = millis();
    rgbOff();

    Serial.println();
    Serial.println("========================================");
    Serial.println("  [START] Test begun! RGB off.");
    Serial.print  ("  runId: "); Serial.println(currentRunId);
    Serial.println("  Watching for patient arrival...");
    Serial.println("========================================");
    return;
  }

  // --- [แก้ข้อ 10] ABORT: Chair ยกเลิก/หมดเวลา/กำลังรีบูต ---
  // เดิมไม่มีคำสั่งนี้ บอร์ดจึงค้างรออยู่แบบนั้นและไฟค้างสีของรอบเก่า
  if (strcmp(in.command, "ABORT") == 0) {
    currentState   = CP_IDLE;
    debounceActive = false;
    showReadyLight();

    Serial.println();
    Serial.println("  [ABORT] ได้รับคำสั่งยกเลิกจาก Chair — กลับสู่สถานะพร้อมใช้งาน");
    Serial.println();
    return;
  }

  // --- FINISH: แสดงผลด้วยไฟ RGB ---
  if (strcmp(in.command, "FINISH") == 0) {
    float finalTime = in.timeSec;
    lastResultSec   = finalTime;
    currentState    = CP_RESULT;
    resultStart     = millis();

    const char* risk = riskLevelOf(finalTime);   // เกณฑ์ 11 / 30 ตามเอกสาร

    Serial.println();
    Serial.println("========================================");
    Serial.println("  TEST RESULT");
    Serial.println("----------------------------------------");
    Serial.print("  Total time  : "); Serial.print(finalTime, 2); Serial.println(" s");
    Serial.print("  Risk level  : "); Serial.println(risk);

    if      (strcmp(risk, "LOW") == 0)      { Serial.println("  RGB display : GREEN");  rgbGreen();  }
    else if (strcmp(risk, "MODERATE") == 0) { Serial.println("  RGB display : YELLOW"); rgbYellow(); }
    else                                    { Serial.println("  RGB display : RED");    rgbRed();    }

    Serial.println("========================================");
    Serial.println();
  }
}

// ==========================================================
// SETUP
// ==========================================================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(RED_PIN,   OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN,  OUTPUT);
  rgbOff();

  Serial.println();
  Serial.println("========================================");
  Serial.println("  TUG Test — Checkpoint Controller (ESP2)");
  Serial.print  ("  Firmware: "); Serial.println(FW_VERSION);
  Serial.println("========================================");

  // ----------------------------------------------------------
  // ① WiFi (ต้องทำก่อน ESP-NOW เสมอ เพื่อให้อยู่ channel เดียวกับ ESP_Chair)
  //    v2 ใช้ WiFi ทำงานจริงด้วย ไม่ใช่แค่ sync channel เหมือนเวอร์ชันเดิม
  // ----------------------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("  [WiFi] กำลังเชื่อมต่อ: ");
  Serial.println(WIFI_SSID);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  Serial.print("  [ESP-NOW] MAC ของบอร์ดนี้ : ");
  Serial.println(WiFi.macAddress());   // เอาไปใส่ในตัวแปร checkpointMAC[] ของ ESP_Chair

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("  [WiFi] ✅ เชื่อมต่อสำเร็จ!");
    Serial.print("  [WiFi] IP Address : "); Serial.println(WiFi.localIP());
    Serial.print("  [WiFi] Channel    : "); Serial.println(WiFi.channel());
    Serial.print("  [WiFi] Signal     : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("  [NTP] กำลัง sync เวลา");
    unsigned long t0 = millis();
    while (getEpoch() == 0 && millis() - t0 < 8000) { delay(300); Serial.print("."); }
    Serial.println();
  } else {
    Serial.println("  [WiFi] ⚠️  เชื่อมต่อไม่สำเร็จ!");
    Serial.println("  ESP-NOW อาจทำงานผิดพลาดหากอยู่คนละ channel กับ ESP_Chair");
    Serial.println("  และเว็บจะมองไม่เห็นสถานะของบอร์ดนี้");
  }

  // ----------------------------------------------------------
  // ② Firebase (สำหรับ heartbeat + รับคำสั่งรีเซ็ต)
  // ----------------------------------------------------------
  if (wifiConnected) {
    firebaseConfig.api_key = FIREBASE_API_KEY;
    auth.user.email        = FIREBASE_EMAIL;
    auth.user.password     = FIREBASE_PASS;
    firebaseConfig.token_status_callback = tokenStatusCallback;

    Firebase.begin(&firebaseConfig, &auth);
    Firebase.reconnectWiFi(true);
    fbdo.setResponseSize(4096);

    Serial.println("  [Firebase] กำลังรับ token... (อาจใช้เวลา 5-10 วินาที)");
    firebaseReady = true;
  }

  // ----------------------------------------------------------
  // ③ ESP-NOW (ต้องหลัง WiFi พร้อมแล้ว)
  // ----------------------------------------------------------
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW initialization failed!");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // [แก้ข้อ 13] เปิดการเข้ารหัส — กันคนยิงแพ็กเก็ตปลอมเข้ามาในระบบ
  esp_now_set_pmk((uint8_t *)ESPNOW_PMK);

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, chairMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = true;
  memcpy(peerInfo.lmk, ESPNOW_LMK, 16);

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ERROR] Failed to add ESP-NOW peer!");
    return;
  }

  Serial.println();
  Serial.println("  [ESP-NOW] ✅ พร้อมแล้ว (เข้ารหัส)");
  Serial.print  ("  [Config] เกณฑ์ความเสี่ยง: LOW <= ");
  Serial.print(TUG_LOW_RISK_MAX, 0); Serial.print("s, MODERATE <= ");
  Serial.print(TUG_MOD_RISK_MAX, 0); Serial.println("s, HIGH > 30s");
  Serial.println("  Firmware ready. Waiting for START...");
  Serial.println("========================================");
  Serial.println();

  showReadyLight();   // ไฟน้ำเงิน = พร้อมใช้งาน
}

// ==========================================================
// MAIN LOOP
// ==========================================================
void loop() {
  float distance = getDistance();
  unsigned long now = millis();

  // --- Periodic status print ---
  if (now - lastPrintTime >= SERIAL_INTERVAL_MS) {
    Serial.print("[");
    Serial.print(getStateName(currentState));
    Serial.print("] Distance: ");
    Serial.print(distance, 1);
    Serial.print(" cm");
    if (distanceHeld) Serial.print(" (hold)");
    if (WiFi.status() != WL_CONNECTED)      Serial.print("  [WiFi: X]");
    if (firebaseReady && !Firebase.ready()) Serial.print("  [Firebase: pending]");
    Serial.println();
    lastPrintTime = now;
  }

  // --- งานที่คุย Firestore ---
  // สำคัญ: ห้ามทำระหว่าง CP_DETECTING เด็ดขาด เพราะ HTTPS จะบล็อกลูปหลายร้อย ms
  // แล้วบอร์ดอาจ "พลาด" จังหวะที่ผู้ทดสอบเดินผ่าน ทำให้เวลา checkpoint เพี้ยน
  if (currentState != CP_DETECTING) {
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
      sendHeartbeat();
      lastHeartbeat = now;
    }
    if (now - lastCommandPoll >= COMMAND_POLL_INTERVAL_MS) {
      checkResetCommand();
      lastCommandPoll = now;
    }
  }

  // --- State Machine ---
  switch (currentState) {

    case CP_IDLE:
      // ไม่ต้องทำอะไร — รอ START ผ่าน ESP-NOW callback
      break;

    case CP_DETECTING:
      // [แก้ข้อ 10] กันค้างถาวรถ้า Chair หายไป (รีบูต/ไฟดับ/แพ็กเก็ต ABORT หาย)
      if (now - detectStart >= DETECT_TIMEOUT_MS) {
        currentState   = CP_IDLE;
        debounceActive = false;
        showReadyLight();
        Serial.println();
        Serial.println("  [TIMEOUT] ⚠️  ไม่มีใครเดินผ่านและ Chair ไม่ตอบ — กลับสู่ IDLE");
        Serial.println();
        break;
      }

      if (distance > 0.0 && distance < DIST_DETECT) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_DETECT_MS) {
          // ยืนยันแล้วว่าผู้ทดสอบมาถึงจุดหมุนตัว
          sendCommand("CHECKPOINT", 0.0);   // แนบ runId ของรอบนี้ไปด้วยอัตโนมัติ
          currentState   = CP_IDLE;
          debounceActive = false;

          Serial.println();
          Serial.println("========================================");
          Serial.println("  [CHECKPOINT] Patient detected!");
          Serial.println("  Signal sent to Chair. Returning to IDLE.");
          Serial.println("========================================");
        }
      } else {
        debounceActive = false;
      }
      break;

    case CP_RESULT:
      // แสดงไฟผลลัพธ์ชั่วคราว แล้วกลับเป็นไฟ "พร้อมใช้งาน"
      // เพื่อให้เจ้าหน้าที่แยกออกว่า "ไฟค้างจากรอบเก่า" กับ "พร้อมทำรอบใหม่" ต่างกัน
      if (now - resultStart >= RESULT_DISPLAY_MS) {
        currentState = CP_IDLE;
        showReadyLight();
        Serial.println("  [RESULT] แสดงผลครบเวลาแล้ว — กลับสู่ไฟพร้อมใช้งาน (น้ำเงิน)");
      }
      break;
  }

  delay(10);
}
