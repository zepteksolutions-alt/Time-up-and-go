// Warns that the chair board is holding results it hasn't managed to upload,
// which means the list on screen is INCOMPLETE. The board retries automatically
// once it's back online.
import { useDeviceStatus } from "../hooks/useDeviceStatus";

// The firmware's on-board queue holds 8 results; past that it drops the oldest,
// so hitting 8 means data may already be lost — escalate the warning.
const BUFFER_LIMIT = 8;

export default function PendingUploadsBanner() {
  const chair = useDeviceStatus("chair");
  if (!chair.known || chair.pendingUploads <= 0) return null;

  const full = chair.pendingUploads >= BUFFER_LIMIT;

  return (
    <div className={`pending-banner ${full ? "pending-banner--critical" : ""}`} role="status">
      <span className="pending-banner__icon" aria-hidden="true">{full ? "⛔" : "⚠️"}</span>
      <div>
        <strong>
          {full
            ? `บอร์ดมีผลค้างเต็มบัฟเฟอร์ (${chair.pendingUploads}/${BUFFER_LIMIT}) — อาจมีข้อมูลสูญหายแล้ว`
            : `มีผลการทดสอบค้างอยู่บนบอร์ด ${chair.pendingUploads} รายการ`}
        </strong>
        <p className="pending-banner__hint">
          {full
            ? "บอร์ดเก็บได้สูงสุด 8 รายการ เกินกว่านั้นจะทิ้งรายการเก่าสุด กรุณาตรวจสอบการเชื่อมต่ออินเทอร์เน็ตของบอร์ดโดยด่วน"
            : "รายการที่แสดงบนหน้าจอยังไม่ครบ — บอร์ดจะส่งให้อัตโนมัติเมื่อกลับมาออนไลน์"}
        </p>
      </div>
    </div>
  );
}
