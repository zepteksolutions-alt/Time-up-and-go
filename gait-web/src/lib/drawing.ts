// Skeleton overlay — the web equivalent of mp_drawing.draw_landmarks in main.py.
import { POSE_CONNECTIONS, type RawLandmark } from "./landmarks";

export function drawSkeleton(
  ctx: CanvasRenderingContext2D,
  landmarks: RawLandmark[] | null,
  width: number,
  height: number,
  minVisibility = 0.5,
  accent = "#38bdf8",
) {
  ctx.clearRect(0, 0, width, height);
  if (!landmarks) return;

  const visible = (i: number) => (landmarks[i]?.visibility ?? 0) >= minVisibility;
  const px = (i: number) => [landmarks[i].x * width, landmarks[i].y * height] as const;

  // Connections
  ctx.lineWidth = 4;
  ctx.strokeStyle = accent;
  ctx.lineCap = "round";
  for (const [a, b] of POSE_CONNECTIONS) {
    if (!visible(a) || !visible(b)) continue;
    const [ax, ay] = px(a);
    const [bx, by] = px(b);
    ctx.beginPath();
    ctx.moveTo(ax, ay);
    ctx.lineTo(bx, by);
    ctx.stroke();
  }

  // Joints
  ctx.fillStyle = "#f8fafc";
  for (let i = 0; i < landmarks.length; i++) {
    if (!visible(i)) continue;
    const [x, y] = px(i);
    ctx.beginPath();
    ctx.arc(x, y, 4, 0, Math.PI * 2);
    ctx.fill();
  }
}
