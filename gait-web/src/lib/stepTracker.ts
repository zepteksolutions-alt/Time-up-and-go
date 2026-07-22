// Real gait-cycle metrics from a frontal camera.
//
// Signal: the METRIC fore-aft separation between the ankles from world
// landmarks (leftAnkle.z − rightAnkle.z, in meters). During walking one foot is
// forward while the other is back, so this signed value swings positive↔negative
// once per STRIDE; each turning point (a foot reaching its forward-most position)
// is one STEP. We count both the maxima and the minima.
//
// Why not the old |Lx − Rx| image separation: for a frontal camera the person
// walks toward the lens, so left/right foot x-positions nearly coincide and that
// signal is tiny and noisy. Fore-aft motion in metric depth is large (≈ the step
// length) and present in every step, including short/shuffling ones.
//
// A ZigZag detector (only reverse after the signal retraces by `floorMeters`)
// plus a refractory period reject standing-still jitter, so cadence stays NaN
// until the person is genuinely walking.

export interface GaitCycleMetrics {
  cadence: number;
  stepTime: number;
  stepTimeVariability: number;
  /**
   * Total steps detected since the tracker was created/reset. Kept separately
   * from `stepTimes`, which is windowed for the cadence estimate and therefore
   * evicts old events — it can never be used as a running total.
   */
  stepCount: number;
}

const EMPTY: Omit<GaitCycleMetrics, "stepCount"> = {
  cadence: NaN,
  stepTime: NaN,
  stepTimeVariability: NaN,
};

export class StepTracker {
  private smooth = NaN;
  private dir = 0; // +1 while tracking an up-swing, -1 a down-swing, 0 = init
  private extreme = NaN; // running extreme of the current swing
  private extremeT = 0; // time of that extreme
  private lastEventT = -Infinity;
  private stepTimes: number[] = []; // step-event timestamps within the window
  private totalSteps = 0; // cumulative, never evicted

  constructor(
    private windowMs: number,
    private minStepIntervalMs: number,
    private floorMeters: number, // retracement (m) needed to confirm a foot reversal
    private emaAlpha = 0.4,
  ) {}

  /** Feed one frame's fore-aft ankle separation in METERS (leftAnkle.z − rightAnkle.z). */
  push(tMs: number, signal: number) {
    if (!Number.isFinite(signal)) return;
    this.smooth = Number.isFinite(this.smooth)
      ? this.emaAlpha * signal + (1 - this.emaAlpha) * this.smooth
      : signal;
    const s = this.smooth;

    if (!Number.isFinite(this.extreme)) {
      this.extreme = s;
      this.extremeT = tMs;
      return;
    }

    if (this.dir >= 0) {
      // Tracking an up-swing → looking for a maximum.
      if (s > this.extreme) {
        this.extreme = s;
        this.extremeT = tMs;
      } else if (this.extreme - s >= this.floorMeters) {
        // Retraced enough: the running max was a real forward-most extreme = a step.
        this.registerStep(this.extremeT);
        this.dir = -1;
        this.extreme = s;
        this.extremeT = tMs;
      }
    } else {
      // Tracking a down-swing → looking for a minimum.
      if (s < this.extreme) {
        this.extreme = s;
        this.extremeT = tMs;
      } else if (s - this.extreme >= this.floorMeters) {
        this.registerStep(this.extremeT);
        this.dir = 1;
        this.extreme = s;
        this.extremeT = tMs;
      }
    }

    const cutoff = tMs - this.windowMs;
    while (this.stepTimes.length && this.stepTimes[0] < cutoff) this.stepTimes.shift();
  }

  private registerStep(tEvent: number) {
    // Refractory: two extremes closer than this are jitter, not two steps.
    if (tEvent - this.lastEventT < this.minStepIntervalMs) return;
    this.stepTimes.push(tEvent);
    this.totalSteps += 1;
    this.lastEventT = tEvent;
  }

  metrics(): GaitCycleMetrics {
    const t = this.stepTimes;
    // The count is always reported — a single detected step is still a fact.
    // Only the RATE estimates need a few events to be meaningful.
    if (t.length < 3) return { ...EMPTY, stepCount: this.totalSteps };
    const spanSec = (t[t.length - 1] - t[0]) / 1000;
    const cadence = spanSec > 0 ? ((t.length - 1) / spanSec) * 60 : NaN;

    const intervals: number[] = [];
    for (let i = 1; i < t.length; i++) intervals.push(t[i] - t[i - 1]);
    const mean = intervals.reduce((a, b) => a + b, 0) / intervals.length;
    const variance = intervals.reduce((a, b) => a + (b - mean) ** 2, 0) / intervals.length;
    const cv = mean > 0 ? (Math.sqrt(variance) / mean) * 100 : NaN;

    return { cadence, stepTime: mean / 1000, stepTimeVariability: cv, stepCount: this.totalSteps };
  }

  reset() {
    this.smooth = NaN;
    this.dir = 0;
    this.extreme = NaN;
    this.extremeT = 0;
    this.lastEventT = -Infinity;
    this.stepTimes = [];
    this.totalSteps = 0;
  }
}
