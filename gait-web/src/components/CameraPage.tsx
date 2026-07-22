import { useCallback, useEffect, useRef, useState } from "react";
import CameraView, { type FrameData } from "./CameraView";
import StatusPanel from "./StatusPanel";
import SummaryModal, { type Summary } from "./SummaryModal";
import { GaitSessionRecorder } from "../lib/recorder";
import { uploadAssessment } from "../lib/firebase";
import { normalizePredictionLabel, type GaitPrediction } from "../lib/classifier";
import { getDiseaseMeta } from "../lib/meta";
import "../camera.css";

const IDLE: GaitPrediction = { status: "No Pose Detected", color: "#f59e0b", reasons: [] };
const IDLE_FRAME: FrameData = { features: null, prediction: IDLE };

// A camera's frame is "live" only if we received one recently — used to tell a
// running side camera apart from one that's off / stalled.
const FRESH_MS = 500;

interface Stamped {
  data: FrameData;
  tMs: number;
}

interface Props {
  activePatientId: string;
  activePatientName: string | null;
}

export default function CameraPage({ activePatientId, activePatientName }: Props) {
  const recorderRef = useRef(new GaitSessionRecorder());
  const recordingRef = useRef(false);
  const frontRef = useRef<Stamped>({ data: IDLE_FRAME, tMs: -Infinity });
  const sideRef = useRef<Stamped>({ data: IDLE_FRAME, tMs: -Infinity });

  const [displayFront, setDisplayFront] = useState<FrameData>(IDLE_FRAME);
  const [displaySide, setDisplaySide] = useState<FrameData>(IDLE_FRAME);
  const [sideLive, setSideLive] = useState(false);
  const [recording, setRecording] = useState(false);
  const [frameCount, setFrameCount] = useState(0);
  const [stepCount, setStepCount] = useState(0);
  const [summary, setSummary] = useState<Summary | null>(null);
  const [uploading, setUploading] = useState(false);

  const handleFront = useCallback((data: FrameData) => {
    frontRef.current = { data, tMs: performance.now() };
  }, []);
  const handleSide = useCallback((data: FrameData) => {
    sideRef.current = { data, tMs: performance.now() };
  }, []);

  // Drive display + fusion at ~10 Hz. Pairing the two independent camera streams
  // on a fixed tick (rather than per-frame) avoids the fps-mismatch sync problem.
  useEffect(() => {
    const id = setInterval(() => {
      const now = performance.now();
      const frontLive = now - frontRef.current.tMs < FRESH_MS;
      const side = now - sideRef.current.tMs < FRESH_MS;

      setDisplayFront(frontLive ? frontRef.current.data : IDLE_FRAME);
      setDisplaySide(side ? sideRef.current.data : IDLE_FRAME);
      setSideLive(side);

      if (recordingRef.current) {
        recorderRef.current.recordFused(
          frontLive ? frontRef.current.data : null,
          side ? sideRef.current.data : null,
        );
        setFrameCount(recorderRef.current.totalFrames);
        setStepCount(recorderRef.current.sessionSteps);
      }
    }, 100);
    return () => clearInterval(id);
  }, []);

  const toggleRecording = () => {
    recorderRef.current.toggle();
    recordingRef.current = recorderRef.current.isRecording;
    setRecording(recorderRef.current.isRecording);
    setFrameCount(recorderRef.current.totalFrames);
    setStepCount(recorderRef.current.sessionSteps);
  };

  const finishAndUpload = async () => {
    recorderRef.current.stop();
    recordingRef.current = false;
    setRecording(false);
    setUploading(true);
    const outcome = await uploadAssessment(recorderRef.current, activePatientId, sideLive ? "front+side" : "front");
    const r = recorderRef.current.result();
    setSummary({
      highestRisk: r.highestRisk,
      riskPercentage: r.riskPercentage,
      totalFrames: recorderRef.current.totalFrames,
      riskScores: { ...recorderRef.current.riskScores },
      stepCount: recorderRef.current.sessionSteps,
      cadenceAvg: recorderRef.current.avgCadence,
      stepTimeCvAvg: recorderRef.current.avgStepTimeVariability,
      uploadStatus: outcome.status,
      documentId: outcome.documentId,
    });
    setUploading(false);
  };

  const hasData = frameCount > 0;

  return (
    <div className="gc-page2">
      <div className="gc-cams">
        <CameraView view="front" label="กล้องด้านหน้า (Front)" onFrame={handleFront} />
        <CameraView view="side" label="กล้องด้านข้าง (Side)" onFrame={handleSide} />
      </div>

      <div className="gc-side">
        <div className="gc-controls">
          <div className="gc-controls__patient">
            ผู้ทดสอบ: <strong>{activePatientName ?? "ไม่ระบุ (เลือกได้ในเมนู ผู้ทดสอบ)"}</strong>
          </div>
          <button className={`gc-btn ${recording ? "gc-btn--danger" : "gc-btn--primary"}`} onClick={toggleRecording}>
            {recording ? `■ หยุดบันทึก (${frameCount})` : "● เริ่มบันทึก"}
          </button>
          <button className="gc-btn" onClick={finishAndUpload} disabled={!hasData || uploading}>
            {uploading ? "กำลังอัปโหลด…" : "จบ & อัปโหลดผล"}
          </button>
          {recording && (
            <span className="gc-rec">
              <span className="gc-rec__dot" />REC · {frameCount} เฟรม · {stepCount} ก้าว
            </span>
          )}
        </div>

        <FusionPanel front={displayFront.prediction} side={displaySide.prediction} sideLive={sideLive} />

        <StatusPanel features={displayFront.features} prediction={displayFront.prediction} />
      </div>

      {summary && <SummaryModal summary={summary} onClose={() => setSummary(null)} />}
    </div>
  );
}

/** Shows what each camera sees and whether they confirm each other. */
function FusionPanel({ front, side, sideLive }: { front: GaitPrediction; side: GaitPrediction; sideLive: boolean }) {
  const fLabel = normalizePredictionLabel(front.status);
  const sLabel = normalizePredictionLabel(side.status);
  const th = (l: string) => getDiseaseMeta(l).th;

  let verdict: { text: string; cls: string };
  if (!sideLive) {
    verdict = { text: "ใช้กล้องเดียว (ด้านหน้า)", cls: "single" };
  } else if (fLabel === sLabel) {
    verdict =
      fLabel === "Normal"
        ? { text: "ตรงกัน: ปกติ", cls: "agree-normal" }
        : { text: `ยืนยันตรงกัน: ${th(fLabel)}`, cls: "agree-risk" };
  } else if (fLabel === "Hemiplegic") {
    verdict = { text: `ยืนยันจากกล้องหน้า: ${th("Hemiplegic")}`, cls: "agree-risk" };
  } else {
    verdict = { text: "สองกล้องยังไม่ยืนยันตรงกัน", cls: "disagree" };
  }

  return (
    <div className="gc-fusion">
      <div className="gc-fusion__cams">
        <span>หน้า: <b>{th(fLabel)}</b></span>
        <span>ข้าง: <b>{sideLive ? th(sLabel) : "—"}</b></span>
      </div>
      <div className={`gc-fusion__verdict gc-fusion__verdict--${verdict.cls}`}>{verdict.text}</div>
    </div>
  );
}
