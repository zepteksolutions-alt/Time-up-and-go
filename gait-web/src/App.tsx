import { useState } from "react";
import Header from "./components/Header";
import { SECTIONS, type SectionKey } from "./components/navigation";
import CameraPage from "./components/CameraPage";
import OverviewSection from "./components/OverviewSection";
import PatientsSection from "./components/PatientsSection";
import RecordsSection from "./components/RecordsSection";
import DiseaseSection from "./components/DiseaseSection";
import GuideSection from "./components/GuideSection";
import PendingUploadsBanner from "./components/PendingUploadsBanner";
import { useTugData } from "./hooks/useTugData";
import "./app-shell.css";
// Loaded last: owns the visual direction (see console.css header).
import "./console.css";

export default function App() {
  const data = useTugData();
  const [activePatientId, setActivePatientId] = useState("");
  const [active, setActive] = useState<SectionKey>("overview");
  const activePatientName = data.patientName(activePatientId);
  const activeIndex = SECTIONS.findIndex((section) => section.key === active);
  const activeSection = SECTIONS[activeIndex];

  const navigate = (key: SectionKey) => {
    setActive(key);
    const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    window.requestAnimationFrame(() => {
      document.getElementById("page-content")?.scrollIntoView({
        behavior: reduceMotion ? "auto" : "smooth",
        block: "start",
      });
    });
  };

  const previousSection = activeIndex > 0 ? SECTIONS[activeIndex - 1] : null;
  const nextSection = activeIndex < SECTIONS.length - 1 ? SECTIONS[activeIndex + 1] : null;

  return (
    <div className="app-shell">
      <Header active={active} onNavigate={navigate} />

      <main id="page-content" className="page-main">
        <PendingUploadsBanner />

        <header className="page-context">
          <div className="page-context__step" aria-hidden="true">
            <strong>{String(activeIndex + 1).padStart(2, "0")}</strong>
            <span>/ {String(SECTIONS.length).padStart(2, "0")}</span>
          </div>
          <div>
            <p className="page-context__eyebrow">{activeSection.eyebrow}</p>
            <h1 className="page-context__title">{activeSection.title}</h1>
            <p className="page-context__description">{activeSection.description}</p>
          </div>
        </header>

        <section
          id="panel-overview"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-overview"
          hidden={active !== "overview"}
        >
          <OverviewSection data={data} />
        </section>
        <section
          id="panel-patients"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-patients"
          hidden={active !== "patients"}
        >
          <PatientsSection
            data={data}
            activePatientId={activePatientId}
            setActivePatientId={setActivePatientId}
          />
        </section>
        <section
          id="panel-camera"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-camera"
          hidden={active !== "camera"}
        >
          <CameraPage activePatientId={activePatientId} activePatientName={activePatientName} />
        </section>
        <section
          id="panel-records"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-records"
          hidden={active !== "records"}
        >
          <RecordsSection data={data} />
        </section>
        <section
          id="panel-disease"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-disease"
          hidden={active !== "disease"}
        >
          <DiseaseSection data={data} />
        </section>
        <section
          id="panel-guide"
          className="page-panel"
          role="tabpanel"
          aria-labelledby="tab-guide"
          hidden={active !== "guide"}
        >
          <GuideSection />
        </section>

        <nav className="page-stepper" aria-label="เปลี่ยนหน้าระบบ">
          <div>
            {previousSection && (
              <button type="button" className="page-stepper__button" onClick={() => navigate(previousSection.key)}>
                <span aria-hidden="true">←</span>
                <span>
                  <small>หน้าก่อนหน้า</small>
                  <strong>{previousSection.label}</strong>
                </span>
              </button>
            )}
          </div>
          <div>
            {nextSection && (
              <button
                type="button"
                className="page-stepper__button page-stepper__button--next"
                onClick={() => navigate(nextSection.key)}
              >
                <span>
                  <small>หน้าถัดไป</small>
                  <strong>{nextSection.label}</strong>
                </span>
                <span aria-hidden="true">→</span>
              </button>
            )}
          </div>
        </nav>
      </main>
    </div>
  );
}
