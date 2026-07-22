// Sticky application header with a keyboard-accessible workflow tab bar.
import { useEffect, type KeyboardEvent } from "react";
import DeviceStatusChip from "./DeviceStatusChip";
import DeviceResetButton from "./DeviceResetButton";
import { SECTIONS, type SectionKey } from "./navigation";

interface Props {
  active: SectionKey;
  onNavigate: (key: SectionKey) => void;
}

export default function Header({ active, onNavigate }: Props) {
  useEffect(() => {
    const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    document.getElementById(`tab-${active}`)?.scrollIntoView({
      behavior: reduceMotion ? "auto" : "smooth",
      block: "nearest",
      inline: "center",
    });
  }, [active]);

  const focusAndNavigate = (key: SectionKey) => {
    onNavigate(key);
    window.requestAnimationFrame(() => document.getElementById(`tab-${key}`)?.focus());
  };

  const handleTabKey = (event: KeyboardEvent<HTMLButtonElement>, key: SectionKey) => {
    const currentIndex = SECTIONS.findIndex((section) => section.key === key);
    let nextIndex: number | null = null;

    if (event.key === "ArrowRight") nextIndex = (currentIndex + 1) % SECTIONS.length;
    if (event.key === "ArrowLeft") nextIndex = (currentIndex - 1 + SECTIONS.length) % SECTIONS.length;
    if (event.key === "Home") nextIndex = 0;
    if (event.key === "End") nextIndex = SECTIONS.length - 1;

    if (nextIndex === null) return;
    event.preventDefault();
    focusAndNavigate(SECTIONS[nextIndex].key);
  };

  return (
    <header className="app-header">
      <div className="app-header__inner">
        <button className="app-brand" type="button" onClick={() => onNavigate("overview")}>
          <span className="app-brand__icon" aria-hidden="true">
            <svg viewBox="0 0 64 64">
              <circle cx="32" cy="32" r="30" fill="currentColor" opacity="0.15" />
              <path d="M28 16h8v12h12v8H36v12h-8V36H16v-8h12V16z" fill="currentColor" />
            </svg>
          </span>
          <span className="app-brand__text">
            <strong>TUG Care Board</strong>
            <small>Timed Up &amp; Go Monitoring</small>
          </span>
        </button>

        <nav className="app-nav" aria-label="หน้าหลักของระบบ" role="tablist">
          {SECTIONS.map(({ key, label }, index) => (
            <button
              key={key}
              id={`tab-${key}`}
              type="button"
              role="tab"
              aria-selected={active === key}
              aria-controls={`panel-${key}`}
              tabIndex={active === key ? 0 : -1}
              className={`app-nav__link ${active === key ? "app-nav__link--active" : ""}`}
              onClick={() => onNavigate(key)}
              onKeyDown={(event) => handleTabKey(event, key)}
            >
              <span className="app-nav__index" aria-hidden="true">{String(index + 1).padStart(2, "0")}</span>
              <span>{label}</span>
            </button>
          ))}
        </nav>

        <div className="app-header__status">
          <DeviceStatusChip deviceId="chair" />
          <DeviceStatusChip deviceId="checkpoint" />
          <DeviceResetButton />
        </div>
      </div>
    </header>
  );
}
