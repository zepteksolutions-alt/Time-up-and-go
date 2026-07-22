import { useState } from "react";
import type { TugData } from "../hooks/useTugData";
import { IconClose, IconPatients, IconPlus, IconUser } from "./Icons";

interface Props {
  data: TugData;
  activePatientId: string;
  setActivePatientId: (id: string) => void;
}

export default function PatientsSection({ data, activePatientId, setActivePatientId }: Props) {
  const { patients, results, assessments, removePatient } = data;
  const [modalOpen, setModalOpen] = useState(false);

  const onDelete = async (id: string, name: string) => {
    if (!confirm(`ต้องการลบผู้ทดสอบ "${name}" จริงหรือไม่?\nผลการทดสอบที่ผูกไว้จะถูกปลดออก`)) return;
    try {
      await removePatient(id);
      if (activePatientId === id) setActivePatientId("");
    } catch (e) {
      alert("เกิดข้อผิดพลาด: " + (e as Error).message);
    }
  };

  return (
    <>
      <section className="patients-section" id="patients">
        <div className="section-header">
          <div>
            <span className="section-header__eyebrow">Patient Management</span>
            <h3 className="section-header__title">จัดการข้อมูลผู้ทดสอบ</h3>
          </div>
          <button className="btn btn--primary" type="button" onClick={() => setModalOpen(true)}>
            <IconPlus width={18} height={18} />
            เพิ่มผู้ทดสอบ
          </button>
        </div>

        <div className="patients-grid">
          {patients.length === 0 ? (
            <div style={{ gridColumn: "1/-1", textAlign: "center", padding: 40, color: "var(--clr-text-secondary)" }}>
              <IconPatients width={44} height={44} style={{ color: "#94a3b8", marginBottom: 8 }} />
              <p style={{ fontSize: ".85rem" }}>ยังไม่มีข้อมูลผู้ทดสอบ — กดปุ่ม "เพิ่มผู้ทดสอบ" เพื่อเริ่มต้น</p>
            </div>
          ) : (
            patients.map((p) => {
              const meta: string[] = [];
              if (p.age) meta.push(`${p.age} ปี`);
              if (p.gender) meta.push(p.gender);
              meta.push(`${results.filter((r) => r.patientId === p.id).length} TUG`);
              meta.push(`${assessments.filter((a) => a.patientId === p.id).length} ประเมินโรค`);
              return (
                <div key={p.id} className={`patient-card ${p.id === activePatientId ? "patient-card--active" : ""}`}>
                  <div className="patient-card__top">
                    <div className="patient-card__avatar">{p.name.charAt(0)}</div>
                    <div className="patient-card__info">
                      <div className="patient-card__name">{p.name}</div>
                      <div className="patient-card__meta">{meta.join(" · ")}</div>
                    </div>
                  </div>
                  {p.note && <div className="patient-card__note">{p.note}</div>}
                  <div className="patient-card__actions">
                    <button className="btn--assign" onClick={() => setActivePatientId(p.id)}>เลือก</button>
                    <button className="btn--danger-sm" onClick={() => onDelete(p.id, p.name)}>ลบ</button>
                  </div>
                </div>
              );
            })
          )}
        </div>
      </section>

      <ActivePatientBar patients={patients} value={activePatientId} onChange={setActivePatientId} />

      {modalOpen && <AddPatientModal data={data} onClose={() => setModalOpen(false)} />}
    </>
  );
}

function ActivePatientBar({
  patients,
  value,
  onChange,
}: {
  patients: TugData["patients"];
  value: string;
  onChange: (id: string) => void;
}) {
  return (
    <div className="active-patient-bar">
      <div className="active-patient-bar__left">
        <IconUser width={20} height={20} />
        <span className="active-patient-bar__label">ผู้ทดสอบที่เลือก:</span>
      </div>
      <select className="active-patient-select" value={value} onChange={(e) => onChange(e.target.value)}>
        <option value="">— ไม่ระบุผู้ทดสอบ —</option>
        {patients.map((p) => (
          <option key={p.id} value={p.id}>{p.name}</option>
        ))}
      </select>
    </div>
  );
}

function AddPatientModal({ data, onClose }: { data: TugData; onClose: () => void }) {
  const [name, setName] = useState("");
  const [age, setAge] = useState("");
  const [gender, setGender] = useState("");
  const [note, setNote] = useState("");
  const [saving, setSaving] = useState(false);

  const submit = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!name.trim()) return;
    setSaving(true);
    try {
      await data.addPatient(name.trim(), age, gender, note.trim());
      onClose();
    } catch (err) {
      alert("เกิดข้อผิดพลาดในการบันทึก: " + (err as Error).message);
      setSaving(false);
    }
  };

  return (
    <div className="modal-overlay modal-overlay--open" onClick={onClose}>
      <div className="modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal__header">
          <h3 className="modal__title">
            <IconPatients width={22} height={22} />
            เพิ่มผู้ทดสอบใหม่
          </h3>
          <button className="modal__close" type="button" aria-label="ปิด" onClick={onClose}>
            <IconClose width={20} height={20} />
          </button>
        </div>
        <form className="modal__form" onSubmit={submit}>
          <div className="form-group">
            <label className="form-label">ชื่อ-นามสกุล <span className="required">*</span></label>
            <input className="form-input" placeholder="เช่น สมชาย ใจดี" required value={name} onChange={(e) => setName(e.target.value)} />
          </div>
          <div className="form-row">
            <div className="form-group">
              <label className="form-label">อายุ (ปี)</label>
              <input className="form-input" type="number" placeholder="65" min={1} max={150} value={age} onChange={(e) => setAge(e.target.value)} />
            </div>
            <div className="form-group">
              <label className="form-label">เพศ</label>
              <select className="form-input" value={gender} onChange={(e) => setGender(e.target.value)}>
                <option value="">— เลือก —</option>
                <option value="ชาย">ชาย</option>
                <option value="หญิง">หญิง</option>
                <option value="อื่นๆ">อื่นๆ</option>
              </select>
            </div>
          </div>
          <div className="form-group">
            <label className="form-label">หมายเหตุ</label>
            <textarea className="form-input form-textarea" rows={2} placeholder="บันทึกเพิ่มเติม เช่น โรคประจำตัว" value={note} onChange={(e) => setNote(e.target.value)} />
          </div>
          <div className="modal__actions">
            <button type="button" className="btn btn--ghost" onClick={onClose}>ยกเลิก</button>
            <button type="submit" className="btn btn--primary" disabled={saving}>{saving ? "กำลังบันทึก…" : "บันทึกข้อมูล"}</button>
          </div>
        </form>
      </div>
    </div>
  );
}
