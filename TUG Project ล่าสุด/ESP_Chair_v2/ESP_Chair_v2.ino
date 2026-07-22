// ============================================================
// ESP_Chair_v2.ino — TUG Test: Start/Finish Line Controller
// ============================================================
// บอร์ดนี้ติดตั้งที่เก้าอี้ (จุดเริ่ม/จุดสิ้นสุด)
//   • ตรวจจับการลุก/นั่งด้วย ultrasonic
//   • จับเวลาการทดสอบ TUG
//   • คุย ESP_Checkpoint ผ่าน ESP-NOW (เข้ารหัส)
//   • อัปโหลดผลขึ้น Cloud Firestore ให้เว็บ https://time-up-and-go.web.app/
//
// Library ที่ต้องติดตั้ง (Arduino Library Manager):
//   → "Firebase ESP Client" by Mobizt
//
// ⚠️  สัญญาข้อมูลกับฝั่งเว็บอธิบายไว้ใน "WEB_DEV_SPEC.txt" — ถ้าแก้ field
//     ในไฟล์นี้ ต้องแก้เอกสารนั้นด้วย
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "secrets.h"  // Copy from secrets.h.example; never commit secrets.h

#define FW_VERSION "chair-2.0.0"

// ============================================================
// ⚙️  USER CONFIGURATION — แก้ไขค่าเหล่านี้ก่อนอัปโหลด
// ============================================================

// WiFi, Firebase and ESP-NOW keys are defined in the ignored secrets.h file.

// ============================================================

// ---------- Pin Configuration ----------
#define TRIG_PIN    18
#define ECHO_PIN    19

// ---------- Distance Thresholds (cm) ----------
#define DIST_SITTING       10.0   // นั่งอยู่ (วัตถุอยู่ภายใน 10 ซม.)
#define DIST_STANDING      30.0   // ลุกขึ้นแล้ว (วัตถุไกลกว่า 30 ซม.)
#define DIST_MAX_VALID    400.0   // ระยะสูงสุดที่เป็นไปได้ของ ultrasonic
#define DIST_TIMEOUT      999.0   // ค่าที่คืนเมื่อไม่มี echo

// ---------- Timing Configuration (ms) ----------
#define DEBOUNCE_SIT_MS    500    // ต้องเห็น "นั่ง" ต่อเนื่อง 500ms จึงยืนยัน
#define DEBOUNCE_STAND_MS  300    // ต้องเห็น "ยืน" ต่อเนื่อง 300ms จึงยืนยัน
#define SERIAL_INTERVAL_MS 500    // ความถี่การพิมพ์สถานะ
#define COOLDOWN_DURATION  15000  // พักระหว่างรอบ (ลดจาก 30s เพราะต้องทำ 3 รอบต่อ session)
#define WIFI_TIMEOUT_MS    10000
#define FIREBASE_TIMEOUT   8000

// [แก้ข้อ 9] เพดานเวลาของการทดสอบหนึ่งรอบ — กันบอร์ดค้างถาวรถ้าผู้ทดสอบล้ม
// เดินออกนอกเส้น หรือไม่กลับมานั่ง เกินเวลานี้จะบันทึกผลเป็น "aborted"
#define MAX_TEST_DURATION_MS  120000   // 120 วินาที

#define HEARTBEAT_INTERVAL_MS    15000  // ส่ง presence heartbeat ทุก 15 วิ
                                        // เว็บถือว่า OFFLINE ถ้า last_seen เก่ากว่า ~3 เท่าของค่านี้
#define COMMAND_POLL_INTERVAL_MS 4000   // ความถี่การเช็คคำสั่งจากเว็บ
#define RETRY_UPLOAD_INTERVAL_MS 20000  // ความถี่การลองส่งผลที่ค้างใน NVS ซ้ำ

// ---------- Median / Valid-only Filter ----------
#define MEDIAN_SAMPLES      5     // จำนวนครั้งที่ยิงเซ็นเซอร์ต่อ 1 รอบ
#define VALID_WINDOW        5     // เก็บค่า valid ล่าสุดกี่ค่าไว้ทำ median

// ---------- Checkpoint Link Heartbeat (ms) ----------
#define PING_INTERVAL_MS           2000
#define CHECKPOINT_LINK_TIMEOUT_MS 5000

// ============================================================
// [แก้ข้อ 1] เกณฑ์ความเสี่ยง TUG — ตามเอกสารโครงการหัวข้อ 6.5.1
//   ≤ 11 วินาที      = LOW      (ไม่มีความเสี่ยง)
//   > 11 – 30 วินาที = MODERATE (มีความเสี่ยง)
//   > 30 วินาที      = HIGH     (เสี่ยงสูงมาก)
// ค่าเดิมในโค้ดคือ 11/20 ซึ่งไม่ตรงเกณฑ์ที่โรงพยาบาลกำหนด
// ⚠️  ค่าคู่นี้ต้องตรงกับ ESP_Checkpoint และกับหน้าเว็บ
// ============================================================
#define TUG_LOW_RISK_MAX   11.0
#define TUG_MOD_RISK_MAX   30.0

// ---------- โหมดการเริ่มทดสอบ ----------
// 0 = เริ่มอัตโนมัติเมื่อผู้ทดสอบลุกจากเก้าอี้ (พฤติกรรมเดิม)
// 1 = ต้องให้เจ้าหน้าที่กด "Start Test" บนเว็บก่อน จึงจะเริ่มจับเวลาได้
//     (ตรงตามเอกสารขั้นตอนที่ 3) — ดูรายละเอียดใน WEB_DEV_SPEC.txt
#define REQUIRE_WEB_START  0

// ---------- ESP-NOW Peer (Checkpoint ESP32) ----------
// ดู MAC ของอีกบอร์ดได้จาก Serial ตอนบูต (บอร์ดจะพิมพ์ MAC ของตัวเองออกมา)
uint8_t checkpointMAC[] = {0x88, 0x57, 0x21, 0x8E, 0xD5, 0x30};

// ---------- Communication Struct ----------
// [แก้ข้อ 10] เพิ่ม runId เพื่อกันแพ็กเก็ตค้างจากรอบก่อนหน้ามาปนรอบปัจจุบัน
// ⚠️  struct นี้ต้องเหมือนกันเป๊ะกับฝั่ง ESP_Checkpoint
typedef struct struct_message {
  char     command[15];
  float    timeSec;
  uint32_t runId;
} struct_message;

// ---------- State Machine ----------
enum SystemState {
  STATE_CALIBRATE,
  STATE_WAIT_SIT,
  STATE_READY,
  STATE_RUNNING,
  STATE_RETURNING,
  STATE_COOLDOWN
};

// ---------- Firebase Objects ----------
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig firebaseConfig;
Preferences    prefs;

// ---------- Global Variables ----------
struct_message      msgData;   // buffer สำหรับ "ส่ง" เท่านั้น
esp_now_peer_info_t peerInfo;

SystemState currentState = STATE_CALIBRATE;

unsigned long startTime       = 0;
unsigned long checkpointTime  = 0;
unsigned long debounceStart   = 0;
unsigned long cooldownStart   = 0;
unsigned long lastPrintTime   = 0;
unsigned long lastHeartbeat   = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastRetryUpload = 0;
unsigned long lastPingTime    = 0;

bool debounceActive = false;
bool wifiConnected  = false;
bool firebaseReady  = false;

uint32_t currentRunId   = 0;    // เลขรอบ ใช้กันแพ็กเก็ตค้าง
uint32_t testStartEpoch = 0;    // เวลาเริ่มทดสอบ (epoch วินาที) ไว้ใส่ในผลลัพธ์

// ---------- [แก้ข้อ 4,5] ข้อมูลผู้เข้าทดสอบ / session / trial ----------
// เว็บเป็นคนกำหนดค่าเหล่านี้ผ่าน device_commands/chair
// ถ้าเว็บยังไม่ได้เขียนมา จะใช้ค่า default เพื่อให้ระบบยังทดสอบได้
char     subjectKey[32] = "unassigned";
char     sessionId[32]  = "unassigned";
uint16_t trialNo        = 1;
bool     armed          = (REQUIRE_WEB_START == 0);  // พร้อมเริ่มจับเวลาหรือยัง

// ---------- Valid-only distance filter ----------
float validWindow[VALID_WINDOW];
int   validHead   = 0;
int   validCount  = 0;
float lastValidDistance = DIST_TIMEOUT;
bool  distanceHeld = false;

// ---------- Checkpoint link status ----------
volatile unsigned long lastCheckpointAck = 0;
bool          checkpointOnlinePrev = false;

// ---------- [แก้ข้อ 11] บัฟเฟอร์ผลที่ยังส่งไม่สำเร็จ (เก็บลง NVS) ----------
// ถ้าเน็ตหลุดตอนจบการทดสอบ ผลจะถูกเก็บลงหน่วยความจำถาวรของ ESP32
// แล้วส่งซ้ำอัตโนมัติเมื่อกลับมาออนไลน์ — ข้อมูลจึงไม่หายแม้ไฟดับ/รีบูต
#define PENDING_MAX 8
typedef struct {
  uint32_t startedAt;      // epoch วินาที
  uint32_t finishedAt;     // epoch วินาที
  float    checkpointSec;
  float    returnSec;
  float    totalSec;
  uint16_t trialNo;
  char     sessionId[32];
  char     subjectKey[32];
  char     status[12];     // "completed" | "aborted"
} PendingResult;

PendingResult pendingBuf[PENDING_MAX];
uint8_t       pendingCount = 0;

// ==========================================================
// Utilities
// ==========================================================
const char* getStateName(SystemState s) {
  switch (s) {
    case STATE_CALIBRATE: return "CALIBRATE";
    case STATE_WAIT_SIT:  return "WAIT_SIT";
    case STATE_READY:     return "READY";
    case STATE_RUNNING:   return "RUNNING";
    case STATE_RETURNING: return "RETURNING";
    case STATE_COOLDOWN:  return "COOLDOWN";
    default:              return "UNKNOWN";
  }
}

// เกณฑ์ความเสี่ยงจุดเดียวในโค้ด — จะได้ไม่มีทางเพี้ยนกันเองระหว่างที่ใช้หลายที่
const char* riskLevelOf(float totalSec) {
  if (totalSec <= TUG_LOW_RISK_MAX) return "LOW";
  if (totalSec <= TUG_MOD_RISK_MAX) return "MODERATE";
  return "HIGH";
}

// เวลาจริงจาก NTP (epoch วินาที) — คืน 0 ถ้ายังไม่ sync
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

// Median filter แบบเอาเฉพาะค่า valid (ไม่เอา 999.0 timeout มาคิดเป็นระยะ)
//   ① ยิงเซ็นเซอร์เป็นชุด เก็บเฉพาะค่าที่ valid (0 < d < DIST_MAX_VALID)
//   ② เก็บลง ring buffer ของค่า valid ล่าสุด แล้วหา median
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

  for (int i = 1; i < validCount; i++) {   // insertion sort
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
// [แก้ข้อ 11] NVS pending-result queue
// ==========================================================
void loadPending() {
  prefs.begin("tug", true);   // read-only
  pendingCount = prefs.getUChar("pcount", 0);
  if (pendingCount > PENDING_MAX) pendingCount = 0;   // ข้อมูลเพี้ยน → ทิ้ง
  if (pendingCount > 0) {
    size_t need = sizeof(PendingResult) * pendingCount;
    if (prefs.getBytes("pbuf", pendingBuf, need) != need) pendingCount = 0;
  }
  prefs.end();

  if (pendingCount > 0) {
    Serial.print("  [Buffer] พบผลที่ยังไม่ได้อัปโหลดค้างอยู่ ");
    Serial.print(pendingCount);
    Serial.println(" รายการ — จะลองส่งใหม่เมื่อออนไลน์");
  }
}

void savePending() {
  prefs.begin("tug", false);  // read-write
  prefs.putUChar("pcount", pendingCount);
  if (pendingCount > 0) {
    prefs.putBytes("pbuf", pendingBuf, sizeof(PendingResult) * pendingCount);
  } else {
    prefs.remove("pbuf");
  }
  prefs.end();
}

void enqueuePending(const PendingResult& r) {
  if (pendingCount >= PENDING_MAX) {
    // คิวเต็ม → ทิ้งรายการเก่าสุด (เลื่อนซ้าย) เพื่อเก็บรายการล่าสุดไว้เสมอ
    for (int i = 1; i < PENDING_MAX; i++) pendingBuf[i - 1] = pendingBuf[i];
    pendingCount = PENDING_MAX - 1;
    Serial.println("  [Buffer] ⚠️  คิวเต็ม — ทิ้งผลที่เก่าที่สุด 1 รายการ");
  }
  pendingBuf[pendingCount++] = r;
  savePending();
}

void dequeueFirstPending() {
  for (int i = 1; i < pendingCount; i++) pendingBuf[i - 1] = pendingBuf[i];
  if (pendingCount > 0) pendingCount--;
  savePending();
}

// ==========================================================
// Cloud Firestore
// ==========================================================

// เขียนผล 1 รอบลง Firestore — คืน true เมื่อสำเร็จ
// [แก้ข้อ 2] document ID ใช้ epoch จาก NTP ไม่ใช่ millis() อีกต่อไป
//            millis() จะกลับไปเริ่มที่ 0 ทุกครั้งที่รีบูต ทำให้ document ทับกันเอง
// [แก้ข้อ 3,4,5,7] เพิ่ม timestamp จริง, subject/session/trial, และ return_sec
bool uploadResult(const PendingResult& r) {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return false;

  FirebaseJson content;
  content.set("fields/subject_key/stringValue",      r.subjectKey);
  content.set("fields/session_id/stringValue",       r.sessionId);
  content.set("fields/trial_no/integerValue",        String((long)r.trialNo));
  content.set("fields/checkpoint_sec/doubleValue",   r.checkpointSec);
  content.set("fields/return_sec/doubleValue",       r.returnSec);
  content.set("fields/total_sec/doubleValue",        r.totalSec);
  content.set("fields/risk_level/stringValue",       riskLevelOf(r.totalSec));
  content.set("fields/status/stringValue",           r.status);
  content.set("fields/started_at/integerValue",      String((long)r.startedAt));
  content.set("fields/finished_at/integerValue",     String((long)r.finishedAt));
  content.set("fields/device/stringValue",           "chair");
  content.set("fields/fw_version/stringValue",       FW_VERSION);

  // docId = <epoch ที่จบ>_<trial> — เรียงตามเวลาได้ และไม่ชนกันแม้รีบูต
  String docPath = "tug_results/" + String((unsigned long)r.finishedAt) +
                   "_" + String((unsigned long)r.trialNo);

  bool ok = Firebase.Firestore.createDocument(
              &fbdo, FIREBASE_PROJECT_ID, "(default)",
              docPath.c_str(), content.raw());

  if (ok) {
    Serial.println("  [Firestore] ✅ บันทึกสำเร็จ : " + docPath);
  } else {
    Serial.println("  [Firestore] ❌ บันทึกไม่สำเร็จ : " + fbdo.errorReason());
  }
  return ok;
}

// พยายามส่งผลที่ค้างอยู่ในคิว (ทีละรายการ กันไม่ให้บล็อกลูปนานเกินไป)
void flushPending() {
  if (pendingCount == 0) return;
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  Serial.print("  [Buffer] กำลังส่งผลที่ค้าง (เหลือ ");
  Serial.print(pendingCount);
  Serial.println(" รายการ)...");

  // ถ้าตอนบันทึกยังไม่มีเวลาจริงจาก NTP (finishedAt = 0) ต้องประทับเวลาให้ก่อนส่ง
  // ไม่งั้น document ID จะกลายเป็น "0_1" และรายการที่ค้างหลายอันจะทับกันเอง
  if (pendingBuf[0].finishedAt == 0) {
    uint32_t nowSec = getEpoch();
    if (nowSec == 0) return;          // ยังไม่รู้เวลาจริง — รอรอบหน้า
    pendingBuf[0].finishedAt = nowSec;
    savePending();
    Serial.println("  [Buffer] ℹ️  ประทับเวลาให้รายการที่บันทึกไว้ตอนยังไม่ sync NTP");
  }

  if (uploadResult(pendingBuf[0])) dequeueFirstPending();
}

// บันทึกผล 1 รอบ: ลองส่งทันที ถ้าไม่สำเร็จให้เก็บลง NVS ไว้ส่งทีหลัง
void recordResult(float cpSec, float returnSec, float totalSec,
                  uint32_t startedAt, const char* status) {
  PendingResult r;
  r.startedAt     = startedAt;
  r.finishedAt    = getEpoch();
  r.checkpointSec = cpSec;
  r.returnSec     = returnSec;
  r.totalSec      = totalSec;
  r.trialNo       = trialNo;
  strncpy(r.sessionId,  sessionId,  sizeof(r.sessionId) - 1);
  r.sessionId[sizeof(r.sessionId) - 1] = '\0';
  strncpy(r.subjectKey, subjectKey, sizeof(r.subjectKey) - 1);
  r.subjectKey[sizeof(r.subjectKey) - 1] = '\0';
  strncpy(r.status, status, sizeof(r.status) - 1);
  r.status[sizeof(r.status) - 1] = '\0';

  // ยังไม่มีเวลาจริงจาก NTP → ใส่ 0 ไว้ก่อน แล้วค่อยส่งตอนออนไลน์
  // (uploadResult จะยังเขียนได้ เว็บแค่ต้องเผื่อกรณี finished_at = 0)
  Serial.println();
  Serial.println("  ----------------------------------------");
  Serial.print("  [Firestore] กำลังบันทึกผล (");
  Serial.print(status);
  Serial.println(")...");

  if (r.finishedAt == 0 || !uploadResult(r)) {
    enqueuePending(r);
    Serial.println("  [Buffer] 💾 เก็บผลไว้ในหน่วยความจำถาวร จะส่งใหม่เมื่อออนไลน์");
  }
  Serial.println("  ----------------------------------------");
  Serial.println();
}

// ---------- Presence heartbeat ----------
// อัปเดต device_status/chair ให้เว็บรู้ว่าบอร์ดยังมีชีวิตอยู่
// patchDocument จะสร้าง document ให้ถ้ายังไม่มี และเขียนทับถ้ามีแล้ว
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  uint32_t nowSec = getEpoch();
  if (nowSec == 0) return;   // ยังไม่ sync NTP — ข้ามไปก่อน

  bool cpOnline = (lastCheckpointAck != 0) &&
                  (millis() - lastCheckpointAck < CHECKPOINT_LINK_TIMEOUT_MS);

  FirebaseJson content;
  content.set("fields/online/booleanValue",             true);
  content.set("fields/last_seen/integerValue",          String((long)nowSec));
  content.set("fields/state/stringValue",               getStateName(currentState));
  content.set("fields/rssi/integerValue",               String((long)WiFi.RSSI()));
  content.set("fields/device/stringValue",              "chair");
  content.set("fields/fw_version/stringValue",          FW_VERSION);
  content.set("fields/uptime_sec/integerValue",         String((long)(millis() / 1000)));
  content.set("fields/checkpoint_online/booleanValue",  cpOnline);
  content.set("fields/pending_uploads/integerValue",    String((long)pendingCount));
  content.set("fields/armed/booleanValue",              armed);
  content.set("fields/subject_key/stringValue",         subjectKey);
  content.set("fields/session_id/stringValue",          sessionId);
  content.set("fields/trial_no/integerValue",           String((long)trialNo));

  if (!Firebase.Firestore.patchDocument(
        &fbdo, FIREBASE_PROJECT_ID, "(default)",
        "device_status/chair", content.raw(),
        "online,last_seen,state,rssi,device,fw_version,uptime_sec,"
        "checkpoint_online,pending_uploads,armed,subject_key,session_id,trial_no")) {
    Serial.println("  [Heartbeat] ❌ " + fbdo.errorReason());
  }
}

// ---------- helper: อ่านค่า integer จาก payload ของ Firestore ----------
long readIntField(FirebaseJson& payload, const char* path) {
  FirebaseJsonData result;
  payload.get(result, path);
  // Firestore ส่ง integerValue มาเป็น "string" เสมอ → แปลงผ่าน String ปลอดภัยกว่า
  return result.success ? result.to<String>().toInt() : 0;
}

void readStringField(FirebaseJson& payload, const char* path, char* dest, size_t len) {
  FirebaseJsonData result;
  payload.get(result, path);
  if (!result.success) return;
  String v = result.to<String>();
  if (v.length() == 0) return;
  strncpy(dest, v.c_str(), len - 1);
  dest[len - 1] = '\0';
}

// forward declarations (ฟังก์ชันเหล่านี้ถูกเรียกก่อนจุดที่นิยามไว้)
void abortTest(const char* reason);
void sendCommand(const char* cmd, float t);

// ---------- [แก้ข้อ 6] คำสั่งจากเว็บ (reset / start / abort + ข้อมูลผู้ทดสอบ) ----------
// อ่าน device_commands/chair ครั้งเดียวได้ครบทุกคำสั่ง (ประหยัด quota กว่าแยกอ่าน)
//
// รูปแบบ ack: ทุกคำสั่งมีคู่ <cmd>_requested_at / <cmd>_handled_at (epoch วินาที)
// เว็บเขียน requested_at, บอร์ดเขียน handled_at กลับเมื่อทำเสร็จ
// ถ้า requested_at > handled_at แปลว่ามีคำสั่งใหม่ที่ยังไม่ได้ทำ
void checkCommands() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                      "device_commands/chair", "")) {
    return;   // ยังไม่มี document (เว็บยังไม่เคยสั่งอะไร) — ไม่ต้องทำอะไร
  }

  FirebaseJson payload;
  payload.setJsonData(fbdo.payload());

  // --- ข้อมูลผู้เข้าทดสอบที่เว็บกำหนดไว้ (อ่านทุกครั้ง ให้ผลลัพธ์ผูกถูกคน) ---
  char newSession[32];
  strncpy(newSession, sessionId, sizeof(newSession));
  readStringField(payload, "fields/subject_key/stringValue", subjectKey,  sizeof(subjectKey));
  readStringField(payload, "fields/session_id/stringValue",  newSession,  sizeof(newSession));

  // [แก้ข้อ 5] session ใหม่ = เริ่มนับ trial ใหม่ตั้งแต่ 1 (เอกสารกำหนดให้ทดสอบ 3 รอบ)
  if (strcmp(newSession, sessionId) != 0) {
    strncpy(sessionId, newSession, sizeof(sessionId));
    trialNo = 1;
    Serial.print("  [Session] เริ่ม session ใหม่: ");
    Serial.print(sessionId);
    Serial.print("  (subject=");
    Serial.print(subjectKey);
    Serial.println(", trial กลับไปเป็น 1)");
  }

  long resetReq = readIntField(payload, "fields/reset_requested_at/integerValue");
  long resetAck = readIntField(payload, "fields/reset_handled_at/integerValue");
  long startReq = readIntField(payload, "fields/start_requested_at/integerValue");
  long startAck = readIntField(payload, "fields/start_handled_at/integerValue");
  long abortReq = readIntField(payload, "fields/abort_requested_at/integerValue");
  long abortAck = readIntField(payload, "fields/abort_handled_at/integerValue");

  // ---------- ABORT: ยกเลิกการทดสอบที่กำลังทำอยู่ ----------
  if (abortReq > 0 && abortReq > abortAck) {
    FirebaseJson ack;
    ack.set("fields/abort_handled_at/integerValue", String(abortReq));
    Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                     "device_commands/chair", ack.raw(),
                                     "abort_handled_at");
    if (currentState == STATE_RUNNING || currentState == STATE_RETURNING) {
      abortTest("ยกเลิกจากเว็บ");
    } else {
      Serial.println("  [Abort] ไม่มีการทดสอบที่กำลังทำอยู่ — ไม่ต้องทำอะไร");
    }
    return;
  }

  // ---------- START: เจ้าหน้าที่กด "Start Test" บนเว็บ ----------
  // ความหมาย = "อนุญาตให้เริ่มจับเวลาได้" (arm) ตัวจับเวลาจริงยังเริ่มตอนผู้ทดสอบลุก
  // เพื่อให้ตรงนิยาม TUG ที่ว่าเริ่มจับเวลาเมื่อเห็นผู้ทดสอบขยับตัวลุกขึ้น
  if (startReq > 0 && startReq > startAck) {
    FirebaseJson ack;
    ack.set("fields/start_handled_at/integerValue", String(startReq));
    Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                     "device_commands/chair", ack.raw(),
                                     "start_handled_at");
    armed = true;
    Serial.println("  [Start] ✅ ได้รับคำสั่งเริ่มจากเว็บ — พร้อมจับเวลาเมื่อผู้ทดสอบลุกขึ้น");
    return;
  }

  // ---------- RESET: รีบูตบอร์ด ----------
  // สำคัญ: ต้องเขียน reset_handled_at ให้สำเร็จ "ก่อน" ESP.restart() เสมอ
  // เพราะ Firestore เป็น state เดียวที่รอดจากการรีบูต (millis() และ RAM ถูกล้างหมด)
  // ถ้ารีบูตโดยยังไม่ ack บอร์ดจะเห็นคำสั่งเดิมค้างอยู่หลังบูต แล้ววนรีบูตไม่รู้จบ
  if (resetReq > 0 && resetReq > resetAck) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("  [Remote Reset] คำสั่งรีเซ็ตจากเว็บ — กำลังรีบูต...");
    Serial.println("========================================");

    FirebaseJson ack;
    ack.set("fields/reset_handled_at/integerValue", String(resetReq));

    // รีบูตเฉพาะเมื่อ ack สำเร็จจริง ถ้า ack ล้มเหลวแล้วรีบูต คำสั่งจะยังค้าง
    // → บูตขึ้นมาเจอคำสั่งเดิม → รีบูตอีก วนไปเรื่อย ๆ จนบอร์ดใช้งานไม่ได้
    if (!Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)",
                                          "device_commands/chair", ack.raw(),
                                          "reset_handled_at")) {
      Serial.println("  [Remote Reset] ❌ ack ไม่สำเร็จ — ยกเลิกการรีบูต จะลองใหม่รอบหน้า");
      Serial.println("  [Remote Reset] Error : " + fbdo.errorReason());
      return;
    }

    // บอก Checkpoint ให้เคลียร์สถานะด้วย ไม่งั้นมันจะค้างรอคนเดินผ่านและไฟค้างสีเดิม
    sendCommand("ABORT", 0.0);
    delay(200);       // ให้ HTTP response และแพ็กเก็ต ESP-NOW ออกไปให้เรียบร้อยก่อน
    ESP.restart();
  }
}

// ==========================================================
// ESP-NOW
// ==========================================================

// ---------- ความเข้ากันได้ของ signature ระหว่าง core เวอร์ชันต่าง ๆ ----------
// ESP32 Arduino core 3.x เปลี่ยน argument ตัวแรกของ callback ทั้งสองตัว
// จาก "uint8_t* MAC" เป็น struct info (esp_now_send_info_t / esp_now_recv_info_t)
//
// โค้ดเดิมใช้วิธี cast ทับ เช่น (esp_now_recv_cb_t)OnDataRecv ซึ่งทำให้คอมไพล์ผ่าน
// แต่จริง ๆ แล้วผิด — ตัวแปรที่รับมาเป็นคนละชนิดกับที่ประกาศไว้
// ที่ยังไม่พังเพราะโค้ดไม่เคยแตะ argument ตัวนั้นเลยเท่านั้น
// ตรงนี้แก้ให้ประกาศถูกชนิดจริง ๆ และลงทะเบียนโดยไม่ต้อง cast
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define ESPNOW_SEND_CB_ARG  const esp_now_send_info_t *sendInfo
  #define ESPNOW_RECV_CB_ARG  const esp_now_recv_info_t *recvInfo
#else
  #define ESPNOW_SEND_CB_ARG  const uint8_t *sendInfo
  #define ESPNOW_RECV_CB_ARG  const uint8_t *recvInfo
#endif

// ใช้เป็น heartbeat: ส่งสำเร็จ (มี ACK ชั้น MAC) = Checkpoint ยังออนไลน์
void OnDataSent(ESPNOW_SEND_CB_ARG, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) lastCheckpointAck = millis();
}

void OnDataRecv(ESPNOW_RECV_CB_ARG, const uint8_t *incomingData, int len) {
  // ใช้ buffer แยกจาก msgData ที่ใช้ส่ง ไม่งั้นข้อมูลขารับจะไปทับข้อมูลขาส่ง
  struct_message in;
  if (len != sizeof(in)) return;
  memcpy(&in, incomingData, sizeof(in));
  in.command[sizeof(in.command) - 1] = '\0';

  if (strcmp(in.command, "CHECKPOINT") == 0 && currentState == STATE_RUNNING) {
    // [แก้ข้อ 10] รับเฉพาะแพ็กเก็ตของรอบปัจจุบัน กันสัญญาณค้างจากรอบก่อน
    if (in.runId != currentRunId) {
      Serial.println("  [CHECKPOINT] ⚠️  ไม่ตรง runId (แพ็กเก็ตค้างจากรอบก่อน) — ไม่นับ");
      return;
    }

    checkpointTime = millis() - startTime;
    currentState   = STATE_RETURNING;

    Serial.println("========================================");
    Serial.println("  [CHECKPOINT] Signal received!");
    Serial.print("  Checkpoint time: ");
    Serial.print(checkpointTime / 1000.0, 2);
    Serial.println(" s");
    Serial.println("  Waiting for patient to return...");
    Serial.println("========================================");

    debounceActive = false;
  }
}

void sendCommand(const char* cmd, float t) {
  strncpy(msgData.command, cmd, sizeof(msgData.command) - 1);
  msgData.command[sizeof(msgData.command) - 1] = '\0';
  msgData.timeSec = t;
  msgData.runId   = currentRunId;
  esp_now_send(checkpointMAC, (uint8_t *)&msgData, sizeof(msgData));
}

// ==========================================================
// Test flow helpers
// ==========================================================
void printFinishSummary(float cpSec, float returnSec, float totalSec) {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  TEST COMPLETE");
  Serial.println("----------------------------------------");
  Serial.print("  Subject / Session : ");
  Serial.print(subjectKey); Serial.print(" / "); Serial.println(sessionId);
  Serial.print("  Trial             : "); Serial.println(trialNo);
  Serial.print("  Checkpoint (ขาไป) : "); Serial.print(cpSec, 2);     Serial.println(" s");
  Serial.print("  Return (ขากลับ)   : "); Serial.print(returnSec, 2); Serial.println(" s");
  Serial.print("  Total time        : "); Serial.print(totalSec, 2);  Serial.println(" s");
  Serial.println("----------------------------------------");
  Serial.print("  Risk level        : ");
  Serial.println(riskLevelOf(totalSec));   // เกณฑ์ 11 / 30 ตามเอกสาร
  Serial.println("========================================");
  Serial.println();
}

void enterCooldown() {
  currentState   = STATE_COOLDOWN;
  cooldownStart  = millis();
  debounceActive = false;
  if (REQUIRE_WEB_START) armed = false;   // รอบถัดไปต้องให้เว็บกด Start อีกครั้ง
  trialNo++;                              // นับรอบถัดไปของ session เดิม

  Serial.print("  Cooldown: ");
  Serial.print(COOLDOWN_DURATION / 1000);
  Serial.println(" seconds before next test...");
}

// [แก้ข้อ 9,10] ยกเลิกการทดสอบ — ใช้ทั้งกรณี timeout และกรณีเว็บสั่ง abort
void abortTest(const char* reason) {
  unsigned long elapsed = millis() - startTime;
  float totalSec = elapsed / 1000.0;
  float cpSec    = checkpointTime / 1000.0;
  float retSec   = (checkpointTime > 0) ? (totalSec - cpSec) : 0.0;

  Serial.println();
  Serial.println("========================================");
  Serial.print("  ⚠️  TEST ABORTED — ");
  Serial.println(reason);
  Serial.print("  เวลาที่ผ่านไปก่อนยกเลิก : ");
  Serial.print(totalSec, 2);
  Serial.println(" s");
  Serial.println("========================================");

  // บอก Checkpoint ให้เลิกเฝ้าและเคลียร์ไฟ ไม่งั้นมันค้างรออยู่แบบนั้นตลอด
  sendCommand("ABORT", 0.0);

  // บันทึกไว้เป็นหลักฐานว่ารอบนี้ล้มเหลว (เพื่อ audit ตามที่เอกสารกำหนด)
  // เว็บต้องกรอง status != "completed" ออกจากการคำนวณค่าเฉลี่ย
  recordResult(cpSec, retSec, totalSec, testStartEpoch, "aborted");

  enterCooldown();
}

// ==========================================================
// SETUP
// ==========================================================
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  TUG Test — Chair Controller (ESP1)");
  Serial.print  ("  Firmware: "); Serial.println(FW_VERSION);
  Serial.println("========================================");

  loadPending();   // กู้ผลที่ค้างจากรอบก่อน (ถ้ามี) ขึ้นมาจาก NVS

  // ----------------------------------------------------------
  // ① WiFi (ต้องทำก่อน ESP-NOW และ Firebase)
  //    ESP-NOW จะใช้ channel เดียวกับ WiFi Router โดยอัตโนมัติ
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
  Serial.println(WiFi.macAddress());   // เอาไปใส่ในตัวแปร chairMAC[] ของ ESP_Checkpoint

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("  [WiFi] ✅ เชื่อมต่อสำเร็จ!");
    Serial.print("  [WiFi] IP Address : "); Serial.println(WiFi.localIP());
    Serial.print("  [WiFi] Channel    : "); Serial.println(WiFi.channel());
    Serial.print("  [WiFi] Signal     : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");

    // NTP — จำเป็นสำหรับ document ID, last_seen และ started_at/finished_at
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("  [NTP] กำลัง sync เวลา");
    unsigned long t0 = millis();
    while (getEpoch() == 0 && millis() - t0 < 8000) { delay(300); Serial.print("."); }
    Serial.println();
    if (getEpoch() == 0) {
      Serial.println("  [NTP] ⚠️  sync ไม่สำเร็จ — ผลจะถูกเก็บใน buffer จนกว่าจะได้เวลาจริง");
    } else {
      Serial.print("  [NTP] ✅ epoch = "); Serial.println(getEpoch());
    }
  } else {
    wifiConnected = false;
    Serial.println("  [WiFi] ⚠️  เชื่อมต่อไม่สำเร็จ!");
    Serial.println("  ระบบยังทดสอบได้ ผลจะถูกเก็บไว้ใน buffer และส่งเมื่อกลับมาออนไลน์");
  }

  // ----------------------------------------------------------
  // ② Firebase (Cloud Firestore)
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
  // ③ ESP-NOW (ต้องทำหลัง WiFi เพื่อให้อยู่ channel เดียวกัน)
  // ----------------------------------------------------------
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW initialization failed!");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  // [แก้ข้อ 13] เปิดการเข้ารหัส ESP-NOW
  // ถ้าไม่เข้ารหัส ใครก็ยิงแพ็กเก็ตปลอม "CHECKPOINT" ใส่บอร์ดนี้ได้
  // ทำให้เวลาที่วัดได้ผิดโดยที่ไม่มีใครรู้ตัว
  esp_now_set_pmk((uint8_t *)ESPNOW_PMK);

  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, checkpointMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = true;
  memcpy(peerInfo.lmk, ESPNOW_LMK, 16);

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ERROR] Failed to add ESP-NOW peer!");
    return;
  }

  Serial.println("  [ESP-NOW] ✅ พร้อมแล้ว (เข้ารหัส)");
  Serial.println("  [Checkpoint] เช็คการเชื่อมต่อผ่าน PING ทุก 2 วิ");
  Serial.print  ("  [Config] เกณฑ์ความเสี่ยง: LOW <= ");
  Serial.print(TUG_LOW_RISK_MAX, 0); Serial.print("s, MODERATE <= ");
  Serial.print(TUG_MOD_RISK_MAX, 0); Serial.println("s, HIGH > 30s");
  Serial.print  ("  [Config] โหมดเริ่มทดสอบ: ");
  Serial.println(REQUIRE_WEB_START ? "ต้องกด Start บนเว็บก่อน" : "อัตโนมัติเมื่อลุกจากเก้าอี้");
  Serial.println("  Firmware ready. Calibrating...");
  Serial.println("========================================");
  Serial.println();

  currentState = STATE_CALIBRATE;
}

// ==========================================================
// MAIN LOOP
// ==========================================================
void loop() {
  float distance = getDistance();
  unsigned long now = millis();

  // ---- ช่วงที่ห้ามคุย Firestore ----
  // การเรียก HTTPS จะบล็อกลูปหลายร้อย ms ถ้าเกิดขึ้นตอนที่ลูปกำลังเฝ้าดูเหตุการณ์
  // ที่ใช้จับเวลา ผลที่วัดได้จะคลาดเคลื่อน
  //
  // มีเฉพาะ STATE_RETURNING เท่านั้นที่อันตราย เพราะการตรวจจับ "นั่งลง" (= จุดหยุดเวลา)
  // ทำอยู่ในลูปนี้ตรง ๆ
  //
  // ส่วน STATE_RUNNING ปลอดภัย เพราะจุดจับเวลา checkpoint คำนวณอยู่ใน OnDataRecv
  // ซึ่งเป็น callback ของ ESP-NOW — ทำงานคนละ context กับลูป จึงไม่ถูกบล็อกไปด้วย
  //
  // ผลคือ: ปุ่ม Abort บนเว็บใช้ได้ตลอดช่วงเดินออก และจะมีดีเลย์แค่ช่วงขากลับสั้น ๆ
  bool timingCritical = (currentState == STATE_RETURNING);

  // --- PING ให้ Checkpoint เป็นระยะ เพื่อเช็คว่ายังเชื่อมต่ออยู่ไหม ---
  //     Checkpoint จะไม่สนใจคำสั่ง "PING" แต่ถ้าส่งสำเร็จแปลว่าได้ ACK ชั้น MAC
  if (now - lastPingTime >= PING_INTERVAL_MS) {
    sendCommand("PING", 0.0);
    lastPingTime = now;
  }

  bool cpOnline = (lastCheckpointAck != 0) &&
                  (now - lastCheckpointAck < CHECKPOINT_LINK_TIMEOUT_MS);

  if (cpOnline != checkpointOnlinePrev) {
    Serial.println(cpOnline
      ? "  [Checkpoint] ✅ เชื่อมต่อแล้ว (ได้รับ ACK)"
      : "  [Checkpoint] ⚠️  ขาดการเชื่อมต่อ (ไม่ได้รับ ACK)");
    checkpointOnlinePrev = cpOnline;
  }

  // --- Periodic status print ---
  if (now - lastPrintTime >= SERIAL_INTERVAL_MS) {
    Serial.print("[");
    Serial.print(getStateName(currentState));
    Serial.print("] Distance: ");
    Serial.print(distance, 1);
    Serial.print(" cm");
    if (distanceHeld) Serial.print(" (hold)");
    Serial.print(cpOnline ? "  [Checkpoint: ONLINE]" : "  [Checkpoint: OFFLINE]");
    if (WiFi.status() != WL_CONNECTED)          Serial.print("  [WiFi: X]");
    if (firebaseReady && !Firebase.ready())     Serial.print("  [Firebase: pending]");
    if (pendingCount > 0) { Serial.print("  [Buffer: "); Serial.print(pendingCount); Serial.print("]"); }
    if (REQUIRE_WEB_START && !armed)            Serial.print("  [รอกด Start จากเว็บ]");
    Serial.println();
    lastPrintTime = now;
  }

  // --- งานที่คุย Firestore ---
  if (!timingCritical) {
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
      sendHeartbeat();
      lastHeartbeat = now;
    }
    if (now - lastCommandPoll >= COMMAND_POLL_INTERVAL_MS) {
      checkCommands();
      lastCommandPoll = now;
    }
    // ส่งผลที่ค้างซ้ำ ทำเฉพาะตอนว่างจริง ๆ ไม่แย่งเวลาช่วงกำลังทดสอบ
    if (pendingCount > 0 && currentState != STATE_RUNNING &&
        now - lastRetryUpload >= RETRY_UPLOAD_INTERVAL_MS) {
      flushPending();
      lastRetryUpload = now;
    }
  }

  // --- State Machine ---
  switch (currentState) {

    // ---- CALIBRATE: อ่านเซนเซอร์ครั้งแรกเพื่อดูว่ามีคนนั่งอยู่ไหม ----
    case STATE_CALIBRATE:
      if (distance <= DIST_SITTING) {
        currentState = STATE_READY;
        Serial.println("  [CALIBRATE] Chair occupied -> READY");
      } else {
        currentState = STATE_WAIT_SIT;
        Serial.println("  [CALIBRATE] Chair empty -> WAIT_SIT");
      }
      debounceActive = false;
      break;

    // ---- WAIT_SIT: เก้าอี้ว่าง รอคนมานั่ง ----
    case STATE_WAIT_SIT:
      if (distance <= DIST_SITTING) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_SIT_MS) {
          currentState   = STATE_READY;
          debounceActive = false;
          Serial.println("  [WAIT_SIT] Person seated -> READY");
          Serial.println("  Waiting for patient to stand up to begin test...");
        }
      } else {
        debounceActive = false;
      }
      break;

    // ---- READY: มีคนนั่งอยู่ รอลุกขึ้น ----
    case STATE_READY:
      // ถ้าเปิด REQUIRE_WEB_START ต้องรอเจ้าหน้าที่กด Start บนเว็บก่อน
      if (!armed) { debounceActive = false; break; }

      if (distance > DIST_STANDING && distance < DIST_MAX_VALID) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_STAND_MS) {
          currentState   = STATE_RUNNING;
          startTime      = now;
          checkpointTime = 0;
          debounceActive = false;
          currentRunId++;                 // รอบใหม่ = runId ใหม่
          testStartEpoch = getEpoch();

          sendCommand("START", 0.0);

          Serial.println();
          Serial.println("========================================");
          Serial.println("  TEST STARTED");
          Serial.print("  Subject: "); Serial.print(subjectKey);
          Serial.print("  Session: "); Serial.print(sessionId);
          Serial.print("  Trial: ");   Serial.println(trialNo);
          Serial.println("  Timer running. Waiting for checkpoint...");
          Serial.println("========================================");
        }
      } else {
        debounceActive = false;
      }
      break;

    // ---- RUNNING: จับเวลาอยู่ กำลังเดินไป checkpoint ----
    case STATE_RUNNING:
      // การตรวจจับ checkpoint ทำใน OnDataRecv
      // [แก้ข้อ 9] แต่ต้องมีเพดานเวลา กันค้างถาวรถ้าไม่มีสัญญาณ checkpoint กลับมาเลย
      if (now - startTime >= MAX_TEST_DURATION_MS) {
        abortTest("หมดเวลา — ไม่ได้รับสัญญาณจาก Checkpoint");
      }
      break;

    // ---- RETURNING: ผ่าน checkpoint แล้ว รอกลับมานั่ง ----
    case STATE_RETURNING:
      if (now - startTime >= MAX_TEST_DURATION_MS) {
        abortTest("หมดเวลา — ผู้ทดสอบไม่ได้กลับมานั่ง");
        break;
      }

      if (distance <= DIST_SITTING) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_SIT_MS) {
          // นั่งลงแล้ว — จบการทดสอบ
          unsigned long finishDuration = now - startTime;
          float totalSec  = finishDuration / 1000.0;
          float cpSec     = checkpointTime / 1000.0;
          float returnSec = totalSec - cpSec;   // [แก้ข้อ 7] split time ขากลับ

          sendCommand("FINISH", totalSec);          // ① ส่งผลไป Checkpoint (โชว์ไฟ RGB)
          printFinishSummary(cpSec, returnSec, totalSec);  // ② สรุปบน Serial
          recordResult(cpSec, returnSec, totalSec,  // ③ บันทึกขึ้น Firestore (หรือ buffer)
                       testStartEpoch, "completed");

          debounceActive = false;
          enterCooldown();
        }
      } else {
        debounceActive = false;
      }
      break;

    // ---- COOLDOWN: พักระหว่างรอบ ----
    case STATE_COOLDOWN:
      if (now - cooldownStart >= COOLDOWN_DURATION) {
        currentState   = STATE_WAIT_SIT;
        debounceActive = false;
        Serial.println();
        Serial.println("  [COOLDOWN] Complete. Ready for next test.");
        Serial.print  ("  รอบถัดไปคือ trial ที่ "); Serial.println(trialNo);
        Serial.println("  Waiting for patient to sit down...");
        Serial.println();
      }
      break;
  }

  delay(10);
}
