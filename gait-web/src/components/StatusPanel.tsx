// Live metrics + current classification — web port of draw_status_panel().
import type { GaitFeatures } from "../lib/gaitFeatures";
import type { GaitPrediction } from "../lib/classifier";

function fmt(v: number | undefined, suffix = "", precision = 1): string {
  if (v === undefined || !Number.isFinite(v)) return "--";
  return `${v.toFixed(precision)}${suffix}`;
}

interface Props {
  features: GaitFeatures | null;
  prediction: GaitPrediction;
}

export default function StatusPanel({ features, prediction }: Props) {
  return (
    <div className="gc-panel" style={{ borderColor: prediction.color }}>
      <div className="gc-panel__status" style={{ color: prediction.color }}>
        {prediction.status}
      </div>
      <div className="gc-panel__disclaimer">เครื่องมือคัดกรอง (MVP) ไม่ใช่การวินิจฉัยทางการแพทย์</div>

      {!features ? (
        <div className="gc-panel__rows">
          <div>Pose: รอให้เห็นข้อต่อเต็มตัว</div>
          <div>Tip: ยืนให้เห็นเต็มตัวในมุมหน้า/ข้าง</div>
        </div>
      ) : (
        <div className="gc-panel__rows">
          <Row label="Knee L/R" value={`${fmt(features.leftKneeAngle, "°")} / ${fmt(features.rightKneeAngle, "°")}`} />
          <Row label="Hip L/R" value={`${fmt(features.leftHipAngle, "°")} / ${fmt(features.rightHipAngle, "°")}`} />
          <Row label="Step length" value={`${fmt(features.stepLength, "", 3)} bh`} />
          <Row label="Arm swing L/R" value={`${fmt(features.leftArmSwing, "", 3)} / ${fmt(features.rightArmSwing, "", 3)}`} />
          <Row label="Arm asymmetry" value={fmt(features.armSwingAsymmetry, "", 3)} />
          <Row label="Leg symmetry" value={`${fmt(features.symmetryIndex, "", 3)}  (weak: ${features.weakSide})`} />
          <Row label="Trunk lean" value={fmt(features.trunkLean, "°")} />
          <Row label="Knee lift L/R" value={`${fmt(features.leftKneeLift, "", 3)} / ${fmt(features.rightKneeLift, "", 3)}`} />
          <Row label="จำนวนก้าว" value={`${features.stepCount} ก้าว`} />
          <Row label="Cadence" value={fmt(features.cadence, " ก้าว/นาที", 0)} />
          <Row label="Step-time CV" value={fmt(features.stepTimeVariability, "%", 1)} />
        </div>
      )}

      {prediction.reasons.length > 0 && (
        <div className="gc-panel__reasons">{prediction.reasons.slice(0, 3).join(" · ")}</div>
      )}
    </div>
  );
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="gc-panel__row">
      <span className="gc-panel__row-label">{label}</span>
      <span className="gc-panel__row-value">{value}</span>
    </div>
  );
}
