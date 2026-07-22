# Time Up and Go Monitoring System

ระบบต้นแบบสำหรับประเมินความเสี่ยงการหกล้มด้วย Timed Up and Go (TUG)
และวิเคราะห์รูปแบบการเดินด้วยกล้องแบบ on-device

## Project structure

- `gait-web/` — React + TypeScript dashboard and MediaPipe gait camera
- `TUG Project ล่าสุด/ESP_Chair_v2/` — current chair controller firmware
- `TUG Project ล่าสุด/ESP_Checkpoint_v2/` — current turnaround checkpoint firmware
- `ESP_Chair/` and `ESP_Checkpoint/` — legacy firmware and dashboard
- `main.py` — original OpenCV + MediaPipe Python prototype
- `TUG Project ล่าสุด/WEB_DEV_SPEC.txt` — Firestore/firmware integration contract

## Web setup

```bash
cd gait-web
cp .env.example .env.local
npm install
npm run dev
```

Fill `.env.local` with a development Firebase account before running. The file
is ignored by Git. A shared Firebase password embedded in frontend code is not
secure for production; use individual staff accounts and restrictive Firestore
rules before clinical deployment.

## ESP32 setup

For each firmware directory:

1. Copy `secrets.h.example` to `secrets.h`.
2. Fill in Wi-Fi, Firebase and (for v2) ESP-NOW values.
3. Keep `secrets.h` local; it is ignored by Git.
4. Flash the matching Chair and Checkpoint firmware.

## Python prototype

```bash
pip install -r requirements.txt
python main.py
```

The optional Firebase Admin upload expects a local `serviceAccountKey.json`.
That file is ignored by Git and must never be committed.

## Clinical status

This repository is a screening/research prototype, not a diagnostic medical
device. Gait thresholds and camera measurements require validation against
labeled clinical data and the intended camera setup before healthcare use.
