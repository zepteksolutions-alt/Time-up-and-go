// Port of GaitSessionRecorder from main.py.
// Tracks frame-level gait classifications during a recording session.
import { type GaitLabel, type GaitPrediction, normalizePredictionLabel } from "./classifier";
import type { GaitFeatures } from "./gaitFeatures";

const LABELS: GaitLabel[] = ["Normal", "Parkinsonian", "Hemiplegic", "Steppage"];

// A specific abnormal pattern is only reported as a risk when it was seen in at
// least this fraction of the assessed frames. With 4 classes, pure chance is
// ~25%, so 40% is meaningfully above noise and requires the pattern to be
// SUSTAINED across ~half the walk rather than flickering for a few frames.
// Below this (or when normal gait was more frequent) the result is reported as
// "no clear abnormal pattern" instead of the largest abnormal count.
// This is a screening heuristic, not a clinically-validated cutoff — tune it
// against labeled walks. Lower ⇒ more sensitive (more flags); higher ⇒ more
// specific (fewer false alarms).
export const RISK_MIN_SHARE = 0.4;

export type RiskScores = Record<GaitLabel, number>;

/** One camera's per-frame output. */
export interface FrameInput {
  features: GaitFeatures | null;
  prediction: GaitPrediction;
}

/**
 * Combine the two cameras' agreed labels into one confirmed label.
 * Side never emits Hemiplegic, so Hemiplegic here is always front's call.
 */
function fuse(front: GaitLabel, side: GaitLabel): GaitLabel {
  if (front === side) return front; // both agree (Normal / Parkinsonian / Steppage)
  if (front === "Hemiplegic") return "Hemiplegic"; // front-only capability
  return "Normal"; // disagreement → no confirmed abnormal pattern
}

export interface SessionResult {
  highestRisk: string;
  riskPercentage: number;
  /** true when a specific abnormal pattern cleared RISK_MIN_SHARE. */
  flagged: boolean;
}

export class GaitSessionRecorder {
  isRecording = false;
  totalFrames = 0; // assessable frames only (valid pose, windows warmed up)
  skippedFrames = 0; // no-pose / warm-up frames excluded from the denominator
  riskScores: RiskScores = emptyScores();

  // features.stepCount counts from when the CAMERA started, not when recording
  // started, so the session total is the delta between the first and last
  // reading seen while recording.
  private stepsAtStart: number | null = null;
  private latestSteps = 0;
  private cadenceSum = 0;
  private cadenceN = 0;
  private cvSum = 0;
  private cvN = 0;

  start() {
    this.isRecording = true;
    this.totalFrames = 0;
    this.skippedFrames = 0;
    this.riskScores = emptyScores();
    this.stepsAtStart = null;
    this.latestSteps = 0;
    this.cadenceSum = 0;
    this.cadenceN = 0;
    this.cvSum = 0;
    this.cvN = 0;
  }

  /** Steps taken during this recording session. */
  get sessionSteps(): number {
    if (this.stepsAtStart === null) return 0;
    return Math.max(0, this.latestSteps - this.stepsAtStart);
  }

  /** Mean cadence over the session (NaN if never established). */
  get avgCadence(): number {
    return this.cadenceN > 0 ? this.cadenceSum / this.cadenceN : NaN;
  }

  /** Mean step-time variability over the session (NaN if never established). */
  get avgStepTimeVariability(): number {
    return this.cvN > 0 ? this.cvSum / this.cvN : NaN;
  }

  stop() {
    this.isRecording = false;
  }

  toggle() {
    if (this.isRecording) this.isRecording = false;
    else this.start();
  }

  /**
   * Record one frame. No-pose and warm-up frames are EXCLUDED from the result —
   * previously they were folded into the "Normal" count, inflating the Normal
   * denominator and deflating the reported disease-risk percentage.
   */
  record(features: GaitFeatures | null, prediction: GaitPrediction) {
    if (!this.isRecording) return;
    this.accumulateGaitMetrics(features);
    const label = this.assessableLabel(features, prediction);
    if (label === null) {
      this.skippedFrames += 1;
      return;
    }
    this.riskScores[label] += 1;
    this.totalFrames += 1;
  }

  /**
   * Fuse a front-view and a side-view frame. A disease score only increments
   * when the two cameras AGREE that the same abnormal pattern is present — which
   * cross-validates the finding and cuts false positives. Capability-aware:
   *   • Parkinsonian / Steppage — both cameras must agree (both can see them).
   *   • Hemiplegic — front only (the side view can't judge L/R; its classifier
   *     never emits Hemiplegic, so front's call stands).
   *   • Disagreement — no confirmed abnormality → counts toward Normal so the
   *     RISK_MIN_SHARE denominator stays honest (not silently dropped).
   * Falls back to single-camera scoring when only one camera is assessable, so
   * the app still works with just the front camera.
   */
  recordFused(front: FrameInput | null, side: FrameInput | null) {
    if (!this.isRecording) return;

    // Step / cadence metrics come from the front camera (its z-signal is tuned
    // for a frontal view); fall back to the side only if front has no features.
    this.accumulateGaitMetrics(front?.features ?? side?.features ?? null);

    const frontLabel = front ? this.assessableLabel(front.features, front.prediction) : null;
    const sideLabel = side ? this.assessableLabel(side.features, side.prediction) : null;

    let label: GaitLabel | null;
    if (frontLabel !== null && sideLabel !== null) label = fuse(frontLabel, sideLabel);
    else label = frontLabel ?? sideLabel; // single-camera fallback (or null if neither)

    if (label === null) {
      this.skippedFrames += 1;
      return;
    }
    this.riskScores[label] += 1;
    this.totalFrames += 1;
  }

  /**
   * Gait-cycle metrics are gathered BEFORE the warm-up/no-pose filter: the step
   * detector runs on its own signal and can register real steps while the
   * classifier's feature windows are still filling. Dropping those would
   * undercount the walk.
   */
  private accumulateGaitMetrics(features: GaitFeatures | null) {
    if (!features) return;
    if (Number.isFinite(features.stepCount)) {
      if (this.stepsAtStart === null) this.stepsAtStart = features.stepCount;
      this.latestSteps = features.stepCount;
    }
    if (Number.isFinite(features.cadence)) {
      this.cadenceSum += features.cadence;
      this.cadenceN += 1;
    }
    if (Number.isFinite(features.stepTimeVariability)) {
      this.cvSum += features.stepTimeVariability;
      this.cvN += 1;
    }
  }

  /** The frame's label, or null if it's a no-pose / warm-up frame (not assessable). */
  private assessableLabel(features: GaitFeatures | null, prediction: GaitPrediction): GaitLabel | null {
    if (!features || prediction.status.includes("No Pose") || isWarmup(features)) return null;
    return normalizePredictionLabel(prediction.status);
  }

  /**
   * The session's outcome. A specific abnormal pattern is reported ONLY when it
   * was both the most frequent abnormal class AND sustained (≥ RISK_MIN_SHARE of
   * frames) AND more frequent than normal gait. Otherwise "Normal" — i.e. the
   * person walked mostly normally, even if some abnormal frames occurred.
   *
   * The old logic reported the largest abnormal count unconditionally, so a walk
   * of 30 Normal / 3 Parkinsonian frames was labeled "Parkinsonian".
   */
  result(): SessionResult {
    if (this.totalFrames <= 0) return { highestRisk: "No Data", riskPercentage: 0, flagged: false };

    let topAbn: GaitLabel = "Parkinsonian";
    let topCount = -1;
    for (const label of LABELS) {
      if (label === "Normal") continue;
      if (this.riskScores[label] > topCount) {
        topCount = this.riskScores[label];
        topAbn = label;
      }
    }

    const normalCount = this.riskScores.Normal;
    const topShare = topCount / this.totalFrames;
    const flagged = topCount > 0 && topCount > normalCount && topShare >= RISK_MIN_SHARE;

    if (flagged) {
      return { highestRisk: topAbn, riskPercentage: topShare * 100, flagged: true };
    }
    return { highestRisk: "Normal", riskPercentage: (normalCount / this.totalFrames) * 100, flagged: false };
  }
}

// Warm-up = pose is present but the time-windowed dynamic features have not
// accumulated enough data yet, so the only available signal is instantaneous
// joint angles — not a trustworthy gait assessment.
function isWarmup(f: GaitFeatures): boolean {
  return (
    !Number.isFinite(f.meanArmSwing) &&
    !Number.isFinite(f.symmetryIndex) &&
    !Number.isFinite(f.leftKneeLift) &&
    !Number.isFinite(f.rightKneeLift)
  );
}

function emptyScores(): RiskScores {
  return { Normal: 0, Parkinsonian: 0, Hemiplegic: 0, Steppage: 0 };
}
