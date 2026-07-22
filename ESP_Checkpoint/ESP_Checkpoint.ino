// ============================================================
// ESP_Checkpoint.ino — TUG Test: Checkpoint & RGB Controller
// ============================================================
// This ESP32 sits at the 3-meter turnaround point.
// It detects when the patient arrives (via ultrasonic sensor),
// sends a CHECKPOINT signal back to ESP_Chair, and displays
// the test result on an RGB LED after receiving FINISH.
//
// WiFi is connected here solely to align the ESP-NOW channel
// with ESP_Chair. Both boards MUST connect to the same network.
// ============================================================

#include <esp_now.h>
#include <WiFi.h>
#include "secrets.h"  // Copy from secrets.h.example; never commit secrets.h

// ============================================================
// ⚙️  USER CONFIGURATION — แก้ไขค่าเหล่านี้ก่อนอัปโหลด
//     ใช้ SSID / Password เดียวกับที่ตั้งใน ESP_Chair.ino
// ============================================================

// WiFi values are defined in the ignored secrets.h file.

// ============================================================

// ---------- Pin Configuration ----------
#define TRIG_PIN    18
#define ECHO_PIN    19

#define RED_PIN     25
#define GREEN_PIN   26
#define BLUE_PIN    27

// ---------- Distance Thresholds (cm) ----------
#define DIST_DETECT        30.0   // Person detected within 30 cm
#define DIST_TIMEOUT      999.0   // Returned when no echo received

// ---------- Timing Configuration (ms) ----------
#define DEBOUNCE_DETECT_MS 300    // Must detect person for 300ms to confirm
#define SERIAL_INTERVAL_MS 500    // Status print interval
#define WIFI_TIMEOUT_MS    10000  // Max wait for WiFi connection (10s)

// ---------- Median Filter ----------
#define MEDIAN_SAMPLES      5     // Number of readings for median filter

// ---------- TUG Risk Thresholds (seconds) ----------
#define TUG_LOW_RISK_MAX   11.0   // <= 11s: Low risk (Green)
#define TUG_MOD_RISK_MAX   20.0   // <= 20s: Moderate risk (Yellow)
                                   // >  20s: High risk (Red)

// ---------- RGB LED (Common Anode: LOW = ON, HIGH = OFF) ----------
#define LED_ON   LOW
#define LED_OFF  HIGH

// ---------- ESP-NOW Peer (Chair ESP32) ----------
uint8_t chairMAC[] = {0x88, 0x57, 0x21, 0xB6, 0x70, 0x84};

// ---------- Communication Struct (must match ESP_Chair) ----------
typedef struct struct_message {
  char command[15];
  float timeSec;
} struct_message;

// ---------- State Machine ----------
enum CheckpointState {
  CP_IDLE,       // Waiting for START command
  CP_DETECTING   // START received — watching for patient arrival
};

// ---------- Global Variables ----------
struct_message msgData;
esp_now_peer_info_t peerInfo;

CheckpointState currentState = CP_IDLE;

unsigned long debounceStart  = 0;
unsigned long lastPrintTime  = 0;
bool debounceActive = false;

// ---------- Utility: State Name for Serial ----------
const char* getStateName(CheckpointState s) {
  switch (s) {
    case CP_IDLE:      return "IDLE";
    case CP_DETECTING: return "DETECTING";
    default:           return "UNKNOWN";
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

// ---------- RGB LED Control ----------
void setRGB(bool r, bool g, bool b) {
  digitalWrite(RED_PIN,   r ? LED_ON : LED_OFF);
  digitalWrite(GREEN_PIN, g ? LED_ON : LED_OFF);
  digitalWrite(BLUE_PIN,  b ? LED_ON : LED_OFF);
}

void rgbOff()    { setRGB(false, false, false); }
void rgbGreen()  { setRGB(false, true,  false); }
void rgbYellow() { setRGB(true,  true,  false); }  // Red + Green = Yellow
void rgbRed()    { setRGB(true,  false, false); }

// ---------- Helper: Send ESP-NOW Message ----------
void sendCommand(const char* cmd, float t) {
  strcpy(msgData.command, cmd);
  msgData.timeSec = t;
  esp_now_send(chairMAC, (uint8_t *)&msgData, sizeof(msgData));
}

// ---------- ESP-NOW: Send Callback ----------
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("  [ESP-NOW] Send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ---------- ESP-NOW: Receive Callback ----------
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  memcpy(&msgData, incomingData, sizeof(msgData));

  // --- Received START: begin detecting patient ---
  if (strcmp(msgData.command, "START") == 0) {
    currentState = CP_DETECTING;
    debounceActive = false;
    rgbOff();

    Serial.println();
    Serial.println("========================================");
    Serial.println("  [START] Test begun! RGB off.");
    Serial.println("  Watching for patient arrival...");
    Serial.println("========================================");
  }

  // --- Received FINISH: display result on RGB LED ---
  else if (strcmp(msgData.command, "FINISH") == 0) {
    float finalTime = msgData.timeSec;

    Serial.println();
    Serial.println("========================================");
    Serial.println("  TEST RESULT");
    Serial.println("----------------------------------------");
    Serial.print("  Total time  : ");
    Serial.print(finalTime, 2);
    Serial.println(" s");

    if (finalTime <= TUG_LOW_RISK_MAX) {
      Serial.println("  Risk level  : LOW");
      Serial.println("  RGB display : GREEN");
      rgbGreen();
    }
    else if (finalTime <= TUG_MOD_RISK_MAX) {
      Serial.println("  Risk level  : MODERATE");
      Serial.println("  RGB display : YELLOW");
      rgbYellow();
    }
    else {
      Serial.println("  Risk level  : HIGH");
      Serial.println("  RGB display : RED");
      rgbRed();
    }

    Serial.println("========================================");
    Serial.println();
  }
}

// ==========================================================
// SETUP
// ==========================================================
void setup() {
  Serial.begin(115200);

  // Ultrasonic pins
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // RGB LED pins
  pinMode(RED_PIN,   OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN,  OUTPUT);
  rgbOff();

  // ----------------------------------------------------------
  // ① เชื่อมต่อ WiFi ก่อน (ต้องทำก่อน ESP-NOW เสมอ)
  //    เพื่อให้ ESP-NOW ใช้ channel เดียวกับ ESP_Chair
  //    ไม่ได้ใช้ WiFi สำหรับส่งข้อมูล — sync channel อย่างเดียว
  // ----------------------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.println();
  Serial.println("========================================");
  Serial.println("  TUG Test — Checkpoint Controller (ESP2)");
  Serial.println("========================================");
  Serial.print("  [WiFi] กำลังเชื่อมต่อ: ");
  Serial.println(WIFI_SSID);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("  [WiFi] ✅ เชื่อมต่อสำเร็จ!");
    Serial.print("  [WiFi] IP Address : ");
    Serial.println(WiFi.localIP());
    Serial.print("  [WiFi] Channel    : ");
    Serial.println(WiFi.channel());
    Serial.print("  [WiFi] Signal     : ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("  [WiFi] ⚠️  เชื่อมต่อไม่สำเร็จ!");
    Serial.println("  ESP-NOW อาจทำงานผิดพลาดหากอยู่คนละ channel กับ ESP_Chair");
    Serial.println("  ตรวจสอบ SSID / Password แล้วรีสตาร์ทอีกครั้ง");
  }

  // ----------------------------------------------------------
  // ② Initialize ESP-NOW (ต้องหลังจาก WiFi พร้อมแล้ว)
  //    จะทำงานบน channel เดียวกับ WiFi ที่เชื่อมอยู่
  // ----------------------------------------------------------
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] ESP-NOW initialization failed!");
    return;
  }

  esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent);
  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);

  memcpy(peerInfo.peer_addr, chairMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ERROR] Failed to add ESP-NOW peer!");
    return;
  }

  Serial.println();
  Serial.println("  [ESP-NOW] ✅ พร้อมแล้ว");
  Serial.println("  Firmware ready. Waiting for START...");
  Serial.println("========================================");
  Serial.println();
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

    // แสดงสถานะ WiFi ด้วย
    if (WiFi.status() != WL_CONNECTED) {
      Serial.print("  [WiFi: DISCONNECTED]");
    }
    Serial.println();

    lastPrintTime = now;
  }

  // --- State Machine ---
  switch (currentState) {

    case CP_IDLE:
      // Nothing to do — waiting for START via ESP-NOW callback
      break;

    case CP_DETECTING:
      if (distance > 0.0 && distance < DIST_DETECT) {
        if (!debounceActive) {
          debounceActive = true;
          debounceStart = now;
        } else if (now - debounceStart >= DEBOUNCE_DETECT_MS) {
          // Patient confirmed at checkpoint
          sendCommand("CHECKPOINT", 0.0);
          currentState = CP_IDLE;
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
  }

  delay(10);
}
