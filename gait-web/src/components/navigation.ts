export type SectionKey = "overview" | "patients" | "camera" | "records" | "disease" | "guide";

// The order mirrors the clinical workflow: review, select a person, measure,
// inspect the records, review disease screening, then consult the guide.
export const SECTIONS: {
  key: SectionKey;
  label: string;
  eyebrow: string;
  title: string;
  description: string;
}[] = [
  {
    key: "overview",
    label: "ภาพรวม",
    eyebrow: "Dashboard overview",
    title: "ภาพรวมการทดสอบ",
    description: "ติดตามผลล่าสุด สัดส่วนความเสี่ยง และสถานะสำคัญของระบบในจุดเดียว",
  },
  {
    key: "patients",
    label: "ผู้ทดสอบ",
    eyebrow: "Patient management",
    title: "จัดการข้อมูลผู้ทดสอบ",
    description: "เพิ่ม เลือก และจัดการผู้ทดสอบก่อนเริ่มการวัดด้วยกล้องหรืออุปกรณ์ TUG",
  },
  {
    key: "camera",
    label: "กล้องทดสอบ",
    eyebrow: "Dual-camera assessment",
    title: "ตรวจการเดินด้วยกล้องสองมุม",
    description: "ใช้กล้องด้านหน้าและด้านข้างร่วมกันเพื่อบันทึกท่าทางและประเมินรูปแบบการเดิน",
  },
  {
    key: "records",
    label: "ผลการทดสอบ",
    eyebrow: "Test records",
    title: "ผลการทดสอบทั้งหมด",
    description: "ค้นหา เปรียบเทียบ และเชื่อมผล TUG แต่ละรอบเข้ากับผู้ทดสอบ",
  },
  {
    key: "disease",
    label: "เสี่ยงโรค",
    eyebrow: "Disease risk screening",
    title: "ผลประเมินความเสี่ยงจากการเดิน",
    description: "ทบทวนสัญญาณเสี่ยงที่ระบบคัดกรองจากรูปแบบการเดินและระดับความมั่นใจ",
  },
  {
    key: "guide",
    label: "วิธีอ่านผล",
    eyebrow: "Interpretation guide",
    title: "เกณฑ์การแปลผล TUG Test",
    description: "ดูช่วงเวลาและความหมายของระดับความเสี่ยงเพื่อใช้ประกอบการติดตามผล",
  },
];
