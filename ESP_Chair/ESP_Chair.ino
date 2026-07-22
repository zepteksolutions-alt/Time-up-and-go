// ============================================================
// ESP_Chair.ino — TUG Test: Start/Finish Line Controller
// ============================================================
// This ESP32 sits at the chair (start/finish point).
// It detects sit/stand transitions via an ultrasonic sensor,
// manages the test timer, communicates with the Checkpoint
// ESP32 via ESP-NOW, and uploads results to Cloud Firestore.
//
// Library required (install via Arduino Library Manager):
//   → "Firebase ESP Client" by Mobizt
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"   // Token generation process info
#include "secrets.h"              // Copy from secrets.h.example; never commit secrets.h

// ============================================================
// ⚙️  USER CONFIGURATION — แก้ไขค่าเหล่านี้ก่อนอัปโหลด
// ============================================================

// WiFi and Firebase values are defined in the ignored secrets.h file.

// ============================================================

// ---------- Pin Configuration ----------
#define TRIG_PIN    18
#define ECHO_PIN    19

// ---------- Distance Thresholds (cm) ----------
#define DIST_SITTING       10.0   // Person seated (object within 10 cm)
#define DIST_STANDING      30.0   // Person has stood up (object beyond 30 cm)
#define DIST_MAX_VALID    400.0   // Max valid ultrasonic range
#define DIST_TIMEOUT      999.0   // Returned when no echo received

// ---------- Timing Configuration (ms) ----------
#define DEBOUNCE_SIT_MS    500    // Must detect "sitting" for 500ms to confirm
#define DEBOUNCE_STAND_MS  300    // Must detect "standing" for 300ms to confirm
#define SERIAL_INTERVAL_MS 500    // Status print interval
#define COOLDOWN_DURATION  30000  // 30 seconds rest between tests
#define WIFI_TIMEOUT_MS    10000  // Max wait for WiFi connection (10s)
#define FIREBASE_TIMEOUT   8000   // Max wait for Firebase ready (8s)
#define HEARTBEAT_INTERVAL_MS 15000 // Presence heartbeat to Firestore (15s).
                                    // Web treats the chair as OFFLINE if the last
                                    // heartbeat is older than ~3x this (see web).
#define COMMAND_POLL_INTERVAL_MS 4000 // How often to check for a remote reset
                                       // command from the web (device_commands/chair).

// ---------- Median Filter ----------
#define MEDIAN_SAMPLES      5     // Number of readings for median filter

// ---------- TUG Risk Thresholds (seconds) ----------
#define TUG_LOW_RISK_MAX   11.0
#define TUG_MOD_RISK_MAX   20.0

// ---------- ESP-NOW Peer (Checkpoint ESP32) ----------
uint8_t checkpointMAC[] = {0x88, 0x57, 0x21, 0x8E, 0xD5, 0x30};

// ---------- Communication Struct ----------
typedef struct struct_message {
  char command[15];
  float timeSec;
} struct_message;

// ---------- State Machine ----------
enum SystemState {
  STATE_CALIBRATE,   // Boot: determine initial chair occupancy
  STATE_WAIT_SIT,    // Chair empty — waiting for person to sit
  STATE_READY,       // Person seated — waiting for them to stand up
  STATE_RUNNING,     // Timer active — person walking to checkpoint
  STATE_RETURNING,   // Checkpoint passed — person walking back
  STATE_COOLDOWN     // Test complete — resting before next round
};

// ---------- Firebase Objects ----------
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig firebaseConfig;

// ---------- Global Variables ----------
struct_message      msgData;
esp_now_peer_info_t peerInfo;

SystemState currentState = STATE_CALIBRATE;

unsigned long startTime      = 0;
unsigned long checkpointTime = 0;
unsigned long debounceStart  = 0;
unsigned long cooldownStart  = 0;
unsigned long lastPrintTime  = 0;
unsigned long lastHeartbeat  = 0;
unsigned long lastCommandPoll = 0;

bool debounceActive = false;
bool wifiConnected  = false;
bool firebaseReady  = false;

// ---------- Utility: State Name for Serial ----------
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

// ---------- Ultrasonic: Single Reading ----------
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

// ---------- Ultrasonic: Median Filter ----------
float getDistance() {
  float readings[MEDIAN_SAMPLES];

  for (int i = 0; i < MEDIAN_SAMPLES; i++) {
    readings[i] = readDistanceRaw();
    if (i < MEDIAN_SAMPLES - 1) delayMicroseconds(500);
  }

  // Simple insertion sort for median
  for (int i = 1; i < MEDIAN_SAMPLES; i++) {
    float key = readings[i];
    int j = i - 1;
    while (j >= 0 && readings[j] > key) {
      readings[j + 1] = readings[j];
      j--;
    }
    readings[j + 1] = key;
  }

  return readings[MEDIAN_SAMPLES / 2];
}

// ---------- Cloud Firestore: Send TUG result ----------
void sendToFirestore(float cpSec, float totalSec) {
  Serial.println();
  Serial.println("  ----------------------------------------");
  Serial.println("  [Firestore] กำลังบันทึกผลการทดสอบ...");

  // ① ตรวจสอบ WiFi ก่อน
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  [Firestore] ⚠️  WiFi หลุด — กำลัง reconnect...");
    WiFi.reconnect();
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("  [Firestore] ❌ WiFi reconnect ไม่สำเร็จ — ข้ามการบันทึก");
      Serial.println("  ----------------------------------------");
      return;
    }
  }

  // ② รอให้ Firebase token พร้อม
  if (!Firebase.ready()) {
    Serial.print("  [Firestore] รอ token");
    unsigned long t = millis();
    while (!Firebase.ready() && millis() - t < FIREBASE_TIMEOUT) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();
    if (!Firebase.ready()) {
      Serial.println("  [Firestore] ❌ Firebase ไม่พร้อม — ข้ามการบันทึก");
      Serial.println("  ----------------------------------------");
      return;
    }
  }

  // ③ กำหนด risk_level
  String riskLevel;
  if      (totalSec <= TUG_LOW_RISK_MAX) riskLevel = "LOW";
  else if (totalSec <= TUG_MOD_RISK_MAX) riskLevel = "MODERATE";
  else                                   riskLevel = "HIGH";

  // ④ สร้าง Firestore document content
  //    Firestore ต้องการ format: fields/<name>/<type>Value
  FirebaseJson content;
  content.set("fields/checkpoint_sec/doubleValue",  cpSec);
  content.set("fields/total_sec/doubleValue",        totalSec);
  content.set("fields/risk_level/stringValue",       riskLevel.c_str());

  // ⑤ กำหนด path: <collection>/<documentId>
  //    ใช้ millis() เป็น document ID เพื่อความไม่ซ้ำกัน
  String docPath = "tug_results/" + String(millis());

  Serial.println("  [Firestore] Collection : tug_results");
  Serial.println("  [Firestore] Document   : " + String(millis()));
  Serial.println("  [Firestore] Data       : cp=" + String(cpSec, 2) +
                 "s  total=" + String(totalSec, 2) +
                 "s  risk=" + riskLevel);

  // ⑥ บันทึกลง Cloud Firestore
  if (Firebase.Firestore.createDocument(
        &fbdo,
        FIREBASE_PROJECT_ID,
        "(default)",            // database ID (default สำหรับ Firestore ปกติ)
        docPath.c_str(),
        content.raw()
      )) {
    Serial.println("  [Firestore] ✅ บันทึกสำเร็จ!");
    Serial.println("  [Firestore] Path : " + fbdo.dataPath());
  } else {
    Serial.println("  [Firestore] ❌ บันทึกไม่สำเร็จ");
    Serial.println("  [Firestore] Error : " + fbdo.errorReason());
  }

  Serial.println("  ----------------------------------------");
  Serial.println();
}

// ---------- Cloud Firestore: Presence heartbeat ----------
// Upserts device_status/chair with a fresh epoch timestamp + live status so the
// web can show whether the chair controller is powered on / connected.
// patchDocument creates the doc if missing and overwrites it otherwise (one
// fixed document, not a new one each time).
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  time_t nowSec = time(nullptr);
  if (nowSec < 100000) return; // NTP not synced yet — skip until we have real time

  FirebaseJson content;
  content.set("fields/online/booleanValue", true);
  content.set("fields/last_seen/integerValue", String((long)nowSec));
  content.set("fields/state/stringValue", getStateName(currentState));
  content.set("fields/rssi/integerValue", String((long)WiFi.RSSI()));
  content.set("fields/device/stringValue", "chair");

  // updateMask lists the fields to write, so patch overwrites exactly these.
  if (!Firebase.Firestore.patchDocument(
        &fbdo, FIREBASE_PROJECT_ID, "(default)",
        "device_status/chair", content.raw(),
        "online,last_seen,state,rssi,device")) {
    Serial.println("  [Heartbeat] ❌ " + fbdo.errorReason());
  }
}

// ---------- Cloud Firestore: Remote reset command ----------
// Polls device_commands/chair for a reset request from the web. The web writes
// reset_requested_at (epoch sec) when the user clicks "รีเซ็ตบอร์ด"; we compare
// it against reset_handled_at (the last request WE already acted on).
//
// IMPORTANT: we write reset_handled_at = reset_requested_at BEFORE calling
// ESP.restart(). Firestore is the only state that survives the reboot (millis()
// and all RAM reset to zero), so without this ack-first write the board would
// see the same still-pending request again right after booting and reboot in
// an infinite loop.
void checkResetCommand() {
  if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;

  if (!Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "(default)", "device_commands/chair", "")) {
    return; // no command doc yet (nobody has ever requested a reset) — nothing to do
  }

  FirebaseJson payload;
  payload.setJsonData(fbdo.payload());
  FirebaseJsonData result;

  payload.get(result, "fields/reset_requested_at/integerValue");
  long requestedAt = result.success ? result.to<int>() : 0;

  payload.get(result, "fields/reset_handled_at/integerValue");
  long handledAt = result.success ? result.to<int>() : 0;

  if (requestedAt <= 0 || requestedAt <= handledAt) return; // nothing new to handle

  Serial.println();
  Serial.println("========================================");
  Serial.println("  [Remote Reset] คำสั่งรีเซ็ตจากเว็บ — กำลังรีบูต...");
  Serial.println("========================================");

  FirebaseJson ack;
  ack.set("fields/reset_handled_at/integerValue", String(requestedAt));

  // Only reboot if the ack actually landed. If we restarted on a FAILED ack the
  // request would still look pending after boot and we would reboot again, and
  // again — a permanently bricked chair on any persistent write failure. Bailing
  // out here just retries on the next poll instead.
  if (!Firebase.Firestore.patchDocument(
          &fbdo, FIREBASE_PROJECT_ID, "(default)",
          "device_commands/chair", ack.raw(), "reset_handled_at")) {
    Serial.println("  [Remote Reset] ❌ ack ไม่สำเร็จ — ยกเลิกการรีบูต จะลองใหม่รอบหน้า");
    Serial.println("  [Remote Reset] Error : " + fbdo.errorReason());
    return;
  }

  delay(200); // let the ack HTTP response fully settle before tearing down WiFi
  ESP.restart();
}

// ---------- ESP-NOW: Send Callback ----------
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("  [ESP-NOW] Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ---------- ESP-NOW: Receive Callback ----------
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  memcpy(&msgData, incomingData, sizeof(msgData));

  if (strcmp(msgData.command, "CHECKPOINT") == 0 && currentState == STATE_RUNNING) {
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

// ---------- Helper: Send ESP-NOW Message ----------
void sendCommand(const char* cmd, float t) {
  strcpy(msgData.command, cmd);
  msgData.timeSec = t;
  esp_now_send(checkpointMAC, (uint8_t *)&msgData, sizeof(msgData));
}

// ---------- Helper: Print Finish Summary ----------
void printFinishSummary(float cpSec, float totalSec) {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  TEST COMPLETE");
  Serial.println("----------------------------------------");
  Serial.print("  Checkpoint time : ");
  Serial.print(cpSec, 2);
  Serial.println(" s");
  Serial.print("  Total time      : ");
  Serial.print(totalSec, 2);
  Serial.println(" s");
  Serial.println("----------------------------------------");

  if (totalSec <= TUG_LOW_RISK_MAX) {
    Serial.println("  Risk level      : LOW (Green)");
  } else if (totalSec <= TUG_MOD_RISK_MAX) {
    Serial.println("  Risk level      : MODERATE (Yellow)");
  } else {
    Serial.println("  Risk level      : HIGH (Red)");
  }

  Serial.println("========================================");
  Serial.println();
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
  Serial.println("========================================");

  // ----------------------------------------------------------
  // ① เชื่อมต่อ WiFi ก่อน (ต้องทำก่อน ESP-NOW และ Firebase)
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

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("  [WiFi] ✅ เชื่อมต่อสำเร็จ!");
    Serial.print("  [WiFi] IP Address : ");
    Serial.println(WiFi.localIP());
    Serial.print("  [WiFi] Channel    : ");
    Serial.println(WiFi.channel());
    Serial.print("  [WiFi] Signal     : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    // NTP time — needed so the heartbeat can write a real 'last_seen' epoch that
    // the web compares against wall-clock now to decide online/offline.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    wifiConnected = false;
    Serial.println("  [WiFi] ⚠️  เชื่อมต่อไม่สำเร็จ!");
    Serial.println("  ระบบยังทำงานได้ แต่จะไม่บันทึกข้อมูลไป Firestore");
  }

  // ----------------------------------------------------------
  // ② Initialize Firebase (Cloud Firestore)
  //    ใช้ Project ID แทน Database URL
  // ----------------------------------------------------------
  if (wifiConnected) {
    firebaseConfig.api_key              = FIREBASE_API_KEY;
    auth.user.email                     = FIREBASE_EMAIL;
    auth.user.password                  = FIREBASE_PASS;

    // Callback แสดงสถานะ token จาก TokenHelper.h
    firebaseConfig.token_status_callback = tokenStatusCallback;

    Firebase.begin(&firebaseConfig, &auth);
    Firebase.reconnectWiFi(true);

    // ตั้งค่า response buffer
    fbdo.setResponseSize(4096);

    Serial.println("  [Firebase] กำลังรับ token... (อาจใช้เวลา 5-10 วินาที)");
    firebaseReady = true;
  }

  // ----------------------------------------------------------
  // ③ Initialize ESP-NOW
  //    ต้องทำหลัง WiFi เพื่อให้อยู่ channel เดียวกัน
  // ----------------------------------------------------------
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW initialization failed!");
    return;
  }

  esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);
  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);

  memcpy(peerInfo.peer_addr, checkpointMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ERROR] Failed to add ESP-NOW peer!");
    return;
  }

  Serial.println("  [ESP-NOW] ✅ พร้อมแล้ว");
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

  // --- Periodic status print ---
  if (now - lastPrintTime >= SERIAL_INTERVAL_MS) {
    Serial.print("[");
    Serial.print(getStateName(currentState));
    Serial.print("] Distance: ");
    Serial.print(distance, 1);
    Serial.print(" cm");

    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("  [WiFi: X]");
    }
    if (firebaseReady && !Firebase.ready()) {
      Serial.print("  [Firebase: pending]");
    }
    Serial.println();

    lastPrintTime = now;
  }

  // --- Presence heartbeat to Firestore ---
  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeat = now;
  }

  // --- Remote reset command from the web (checked in EVERY state, including
  //     mid-test, since the whole point is to recover a board stuck anywhere) ---
  if (now - lastCommandPoll >= COMMAND_POLL_INTERVAL_MS) {
    checkResetCommand();
    lastCommandPoll = now;
  }

  // --- State Machine ---
  switch (currentState) {

    // ---- CALIBRATE: Read sensor once to determine initial state ----
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

    // ---- WAIT_SIT: Chair empty, waiting for person to sit down ----
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

    // ---- READY: Person is seated, waiting for them to stand ----
    case STATE_READY:
      if (distance > DIST_STANDING && distance < DIST_MAX_VALID) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_STAND_MS) {
          // Person stood up — start the test
          currentState   = STATE_RUNNING;
          startTime      = now;
          checkpointTime = 0;
          debounceActive = false;

          sendCommand("START", 0.0);

          Serial.println();
          Serial.println("========================================");
          Serial.println("  TEST STARTED");
          Serial.println("  Timer running. Waiting for checkpoint...");
          Serial.println("========================================");
        }
      } else {
        debounceActive = false;
      }
      break;

    // ---- RUNNING: Timer active, walking to checkpoint ----
    case STATE_RUNNING:
      // Checkpoint detection handled by OnDataRecv callback
      break;

    // ---- RETURNING: Checkpoint passed, waiting for person to sit back ----
    case STATE_RETURNING:
      if (distance <= DIST_SITTING) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart  = now;
        } else if (now - debounceStart >= DEBOUNCE_SIT_MS) {
          // Person sat back down — test complete
          unsigned long finishDuration = now - startTime;
          float totalSec = finishDuration / 1000.0;
          float cpSec    = checkpointTime / 1000.0;

          // ① ส่งผลไป ESP_Checkpoint (ESP-NOW)
          sendCommand("FINISH", totalSec);

          // ② แสดงสรุปบน Serial Monitor
          printFinishSummary(cpSec, totalSec);

          // ③ บันทึกผลขึ้น Cloud Firestore
          sendToFirestore(cpSec, totalSec);

          // เข้า cooldown
          currentState   = STATE_COOLDOWN;
          cooldownStart  = now;
          debounceActive = false;

          Serial.print("  Cooldown: ");
          Serial.print(COOLDOWN_DURATION / 1000);
          Serial.println(" seconds before next test...");
        }
      } else {
        debounceActive = false;
      }
      break;

    // ---- COOLDOWN: Rest period between tests ----
    case STATE_COOLDOWN:
      if (now - cooldownStart >= COOLDOWN_DURATION) {
        currentState   = STATE_WAIT_SIT;
        debounceActive = false;
        Serial.println();
        Serial.println("  [COOLDOWN] Complete. Ready for next test.");
        Serial.println("  Waiting for patient to sit down...");
        Serial.println();
      }
      break;
  }

  delay(10);
}
