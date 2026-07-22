// Single source of truth for the TUG fall-risk thresholds.
//
// Must stay in sync with riskLevelOf() in ESP_Chair_v2.ino (line ~206):
//   TUG_LOW_RISK_MAX 11.0 / TUG_MOD_RISK_MAX 30.0
//
// Firmware v1 used 30 -> 20, which mis-classified anyone in the 20-30s band as
// HIGH. The hospital criteria (project doc 6.5.1) are:
//   <= 11s      ไม่มีความเสี่ยงต่อการหกล้ม
//   > 11 - 30s  มีความเสี่ยงต่อการหกล้ม
//   > 30s       มีความเสี่ยงสูงมากต่อการหกล้ม
export const TUG_LOW_MAX = 11;
export const TUG_MOD_MAX = 30;

export type RiskLevel = "LOW" | "MODERATE" | "HIGH";

/**
 * Derive the risk level from the measured time.
 *
 * We deliberately RECOMPUTE this instead of trusting the stored `risk_level`
 * field: rows written by firmware v1 were saved with the old 20s cutoff, so
 * trusting the stored value would mix two different criteria in one table.
 * For v2 rows this yields exactly what the board wrote (same thresholds).
 */
export function riskLevelOf(totalSec: number): RiskLevel {
  if (!Number.isFinite(totalSec)) return "LOW";
  if (totalSec <= TUG_LOW_MAX) return "LOW";
  if (totalSec <= TUG_MOD_MAX) return "MODERATE";
  return "HIGH";
}
