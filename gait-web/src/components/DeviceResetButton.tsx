// Remote-reboot button for the ESP32 chair controller. Only usable while the
// chair is online — a board that isn't polling Firestore can't be woken by a
// write, so there's no point queuing a reset for an offline device.
import { useDeviceStatus } from "../hooks/useDeviceStatus";
import { useDeviceReset } from "../hooks/useDeviceReset";

export default function DeviceResetButton() {
  const { online } = useDeviceStatus("chair");
  const { pending, requestReset } = useDeviceReset();

  const onClick = () => {
    const ok = confirm(
      "รีเซ็ต ESP32 (เก้าอี้) ทันที?\n" +
        "การทดสอบที่กำลังทำอยู่ (ถ้ามี) จะถูกยกเลิก และบอร์ดจะออฟไลน์ชั่วคราว ~10-15 วินาที",
    );
    if (ok) requestReset();
  };

  return (
    <button
      type="button"
      className="device-reset-btn"
      disabled={!online || pending}
      onClick={onClick}
      title={!online ? "ต้องให้บอร์ดออนไลน์ก่อนถึงจะสั่งรีเซ็ตได้" : "สั่งรีบูต ESP32 จากระยะไกล"}
      aria-label={pending ? "กำลังส่งคำสั่งรีเซ็ตบอร์ด" : "รีเซ็ตบอร์ด"}
    >
      {/* Full label on roomy viewports; on phones the header row cannot fit
          it beside two status chips, so it collapses to the glyph while the
          accessible name above stays complete. */}
      <span className="device-reset-btn__full">{pending ? "กำลังส่งคำสั่ง…" : "รีเซ็ตบอร์ด"}</span>
      <span className="device-reset-btn__icon" aria-hidden="true">⟳</span>
    </button>
  );
}
