// Inline SVG icons reused across the UI (from web_dashboard markup).
import type { SVGProps } from "react";

const s = (props: SVGProps<SVGSVGElement>) => ({ viewBox: "0 0 24 24", ...props });

export const IconHome = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}><path d="M3 13.2L12 4l9 9.2V21a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1v-7.8z" fill="currentColor" /></svg>
);

export const IconPatients = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
    <circle cx="9" cy="7" r="4" fill="none" stroke="currentColor" strokeWidth="1.8" />
    <path d="M19 8v6m3-3h-6" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
  </svg>
);

export const IconRecords = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <path d="M6 3h9l5 5v12a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinejoin="round" />
    <path d="M14 3v6h6" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinejoin="round" />
    <path d="M8 13h8M8 17h5" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
  </svg>
);

export const IconDisease = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <path d="M4 13h3l2-6 4 12 2.5-7H20" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" />
    <circle cx="18" cy="6" r="2" fill="currentColor" opacity="0.22" />
  </svg>
);

export const IconGuide = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" strokeWidth="1.8" />
    <path d="M12 10.5v5m0-8.5h.01" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" />
  </svg>
);

export const IconCamera = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <path d="M3 8a2 2 0 0 1 2-2h2l1.2-2h7.6L19 6h0a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8z" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinejoin="round" />
    <circle cx="12" cy="12.5" r="3.5" fill="none" stroke="currentColor" strokeWidth="1.8" />
  </svg>
);

export const IconPlus = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}><path d="M12 5v14m-7-7h14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" /></svg>
);

export const IconClose = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}><path d="M18 6L6 18M6 6l12 12" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" /></svg>
);

export const IconUser = (p: SVGProps<SVGSVGElement>) => (
  <svg {...s(p)}>
    <circle cx="12" cy="8" r="4" fill="none" stroke="currentColor" strokeWidth="1.8" />
    <path d="M4 21v-1a6 6 0 0 1 12 0v1" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" />
  </svg>
);
