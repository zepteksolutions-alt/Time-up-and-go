// Central knobs ported from main.py's argparse defaults, plus web-only paths.
export const POSE_CONFIG = {
  minVisibility: 0.6,
  // Time-based feature window. The original used a fixed 45-SAMPLE window, which
  // made every dynamic feature (arm swing, asymmetry, knee lift…) depend on the
  // device frame rate — a 24fps phone and a 60fps laptop measured different
  // things from the same walk. 1500ms ≈ the old 45 samples at 30fps, so
  // thresholds tuned at 30fps stay valid while other devices stay consistent.
  windowMs: 1500,
  minReadyMs: 300, // need this much data before a windowed feature is trusted
  // EMA time constant (seconds). tau 0.065 ≈ the old per-frame alpha 0.4 at 30fps,
  // so smoothing lag is the same wall-clock time on any device.
  emaTauSeconds: 0.065,
  // Majority-vote duration for prediction smoothing (was 9 frames ≈ 300ms@30fps).
  voteMs: 300,
  // Body-height normalizer is taken as the MEDIAN over this window (not the
  // per-frame value) to stop jitter from drifting every normalized feature.
  bodyHeightWindowMs: 700,
  // Below this normalized swing range both sides are "standing still"; asymmetry
  // is then undefined (NaN) instead of exploding toward random large values.
  swingFloor: 0.03,
  // ── Real gait-cycle metrics (cadence / step-time variability) ──
  // Derived from ankle-separation peaks over this longer window (needs several
  // strides). Step-time variability is a validated fall-risk marker.
  gaitWindowMs: 4000,
  minStepIntervalMs: 260, // refractory between step events (~max 230 steps/min)
  // Step detection now runs on the METRIC fore-aft ankle separation from world
  // landmarks (leftAnkle.z − rightAnkle.z, in meters), which oscillates once per
  // stride with an amplitude ≈ the step length. A foot reversal is only counted
  // once the signal retraces by at least this many meters — big enough to reject
  // standing-still jitter (world-z is MediaPipe's noisiest axis), small enough to
  // still catch short shuffling steps (~0.12 m). Simulation: normal + shuffle
  // walks counted exactly, zero false steps standing up to ~0.05 m z-noise.
  // ⚠️ Needs validation against hand-counted real walks — lower to catch tinier
  // steps, raise if a stationary patient accrues phantom steps.
  stepFloorMeters: 0.1,
  // 'lite' | 'full' | 'heavy' — analogous to MediaPipe model_complexity 0/1/2.
  modelVariant: "full" as "lite" | "full" | "heavy",
  minPoseDetectionConfidence: 0.5,
  minTrackingConfidence: 0.5,
};

// MediaPipe Tasks WASM bundle + model assets, served from the public CDN so no
// build-time asset copying is needed. Swap to self-hosted paths for offline use.
const TASKS_VERSION = "0.10.35";
export const WASM_BASE = `https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@${TASKS_VERSION}/wasm`;

export const MODEL_URLS: Record<typeof POSE_CONFIG.modelVariant, string> = {
  lite: "https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_lite/float16/1/pose_landmarker_lite.task",
  full: "https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_full/float16/1/pose_landmarker_full.task",
  heavy: "https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_heavy/float16/1/pose_landmarker_heavy.task",
};

// Camera role in the (future) two-camera rig. Front and side measure
// complementary gait dimensions — see the README.
export type CameraView = "front" | "side";
