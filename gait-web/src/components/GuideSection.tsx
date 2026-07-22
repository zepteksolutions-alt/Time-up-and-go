const GUIDE = [
  {
    cls: "low",
    title: "ความเสี่ยงต่ำ",
    desc: <>เวลารวม <strong>ไม่เกิน 11 วินาที</strong> — มักบ่งชี้ว่าการเคลื่อนไหวอยู่ในเกณฑ์ดี ผู้ทดสอบมีความมั่นคงในการทรงตัว</>,
  },
  {
    cls: "mod",
    title: "ความเสี่ยงปานกลาง",
    desc: <>เวลารวม <strong>มากกว่า 11 แต่ไม่เกิน 30 วินาที</strong> — มีความเสี่ยงต่อการหกล้ม ควรติดตามต่อเนื่อง อาจต้องปรึกษาแพทย์เพิ่มเติม</>,
  },
  {
    cls: "high",
    title: "ความเสี่ยงสูง",
    desc: <>เวลารวม <strong>มากกว่า 30 วินาที</strong> — มีความเสี่ยงสูงมากต่อการหกล้ม ควรประเมินร่วมกับบุคลากรทางการแพทย์โดยเร็ว</>,
  },
];

export default function GuideSection() {
  return (
    <section className="guide-section" id="guide">
      <div className="section-header">
        <div>
          <span className="section-header__eyebrow">Interpretation Guide</span>
          <h3 className="section-header__title">เกณฑ์การแปลผล TUG Test</h3>
        </div>
      </div>
      <div className="guide-grid">
        {GUIDE.map((g) => (
          <article key={g.cls} className={`guide-card guide-card--${g.cls}`}>
            <div>
              <h4 className="guide-card__title">{g.title}</h4>
              <p className="guide-card__desc">{g.desc}</p>
            </div>
          </article>
        ))}
      </div>
      <footer className="footer">
        <p>TUG Care Board - Timed Up &amp; Go Monitoring System © 2026</p>
      </footer>
    </section>
  );
}
