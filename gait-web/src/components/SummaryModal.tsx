// Session summary — web port of draw_summary_modal().
import type { RiskScores } from "../lib/recorder";

export interface Summary {
  highestRisk: string;
  riskPercentage: number;
  totalFrames: number;
  riskScores: RiskScores;
  stepCount: number;
  cadenceAvg: number;
  stepTimeCvAvg: number;
  uploadStatus: string;
  documentId: string | null;
}

export default function SummaryModal({ summary, onClose }: { summary: Summary; onClose: () => void }) {
  const ok = summary.uploadStatus.startsWith("Successfully");
  return (
    <div className="gc-modal-backdrop" onClick={onClose}>
      <div className="gc-modal" onClick={(e) => e.stopPropagation()}>
        <h2 className="gc-modal__title">สรุปผลการวิเคราะห์การเดิน</h2>
        <div className="gc-modal__rows">
          <div className="gc-modal__highlight">
            ผลที่พบเด่นสุด: <b>{summary.highestRisk}</b> ({summary.riskPercentage.toFixed(1)}%)
          </div>
          <div>จำนวนเฟรมที่ประเมิน: {summary.totalFrames}</div>
          <div className="gc-modal__gait">
            <span><b>{summary.stepCount}</b> ก้าว</span>
            <span>
              จังหวะ:{" "}
              <b>{Number.isFinite(summary.cadenceAvg) ? `${summary.cadenceAvg.toFixed(0)} ก้าว/นาที` : "—"}</b>
            </span>
            <span>
              ความแปรปรวน:{" "}
              <b>{Number.isFinite(summary.stepTimeCvAvg) ? `${summary.stepTimeCvAvg.toFixed(1)}%` : "—"}</b>
            </span>
          </div>
          <div className="gc-modal__scores">
            <span>Normal: {summary.riskScores.Normal}</span>
            <span>Parkinsonian: {summary.riskScores.Parkinsonian}</span>
            <span>Hemiplegic: {summary.riskScores.Hemiplegic}</span>
            <span>Steppage: {summary.riskScores.Steppage}</span>
          </div>
          <div className={ok ? "gc-modal__status-ok" : "gc-modal__status-err"}>
            {ok ? "อัปโหลดขึ้น Firebase สำเร็จ" : summary.uploadStatus}
          </div>
        </div>
        <button className="gc-btn gc-btn--primary" onClick={onClose}>
          ปิด / ทดสอบต่อ
        </button>
      </div>
    </div>
  );
}
