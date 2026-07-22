// Thin wrapper around @mediapipe/tasks-vision PoseLandmarker.
// Replaces the `with mp_pose.Pose(...)` block in main.py. Runs in the browser
// on GPU (WebGL/WebGPU) via WASM, so all inference stays on-device (PDPA).
import { FilesetResolver, PoseLandmarker } from "@mediapipe/tasks-vision";
import { MODEL_URLS, POSE_CONFIG, WASM_BASE } from "./config";
import type { RawLandmark } from "./landmarks";

export interface PoseResult {
  landmarks: RawLandmark[] | null;
  // Metric (meters), hip-centered, perspective-corrected 3D landmarks. Used for
  // joint ANGLES, which are far more accurate than angles from the perspective-
  // distorted image landmarks. Null if the model didn't return them.
  worldLandmarks: RawLandmark[] | null;
}

let filesetPromise: ReturnType<typeof FilesetResolver.forVisionTasks> | null = null;

export class PoseEngine {
  private landmarker: PoseLandmarker | null = null;
  private lastTimestamp = -1;

  async init(): Promise<void> {
    if (!filesetPromise) filesetPromise = FilesetResolver.forVisionTasks(WASM_BASE);
    const fileset = await filesetPromise;
    this.landmarker = await PoseLandmarker.createFromOptions(fileset, {
      baseOptions: {
        modelAssetPath: MODEL_URLS[POSE_CONFIG.modelVariant],
        delegate: "GPU",
      },
      runningMode: "VIDEO",
      numPoses: 1,
      minPoseDetectionConfidence: POSE_CONFIG.minPoseDetectionConfidence,
      minPosePresenceConfidence: POSE_CONFIG.minTrackingConfidence,
      minTrackingConfidence: POSE_CONFIG.minTrackingConfidence,
    });
  }

  /** Run pose estimation on a video frame. timestampMs must be monotonic. */
  detect(video: HTMLVideoElement, timestampMs: number): PoseResult {
    if (!this.landmarker) return { landmarks: null, worldLandmarks: null };
    // MediaPipe rejects non-increasing timestamps; guard against duplicate rAF.
    if (timestampMs <= this.lastTimestamp) return { landmarks: null, worldLandmarks: null };
    this.lastTimestamp = timestampMs;

    const out = this.landmarker.detectForVideo(video, timestampMs);
    const landmarks = (out.landmarks?.[0] ?? null) as RawLandmark[] | null;
    const worldLandmarks = (out.worldLandmarks?.[0] ?? null) as RawLandmark[] | null;
    return { landmarks, worldLandmarks };
  }

  close() {
    this.landmarker?.close();
    this.landmarker = null;
  }
}
