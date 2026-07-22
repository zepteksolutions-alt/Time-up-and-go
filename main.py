"""
Real-Time Clinical Gait Analysis MVP

This program uses OpenCV + MediaPipe Pose to estimate 33 body landmarks,
extracts frame-wise and short-window gait features, and classifies gait status
with transparent rule-based clinical heuristics.

Important:
    This is a screening/research prototype, not a diagnostic medical device.
    Thresholds must be calibrated against camera setup, patient population, and
    labeled clinical data before any healthcare use.

Usage:
    python main.py                       # Webcam 0
    python main.py --source 1            # Webcam 1
    python main.py --source path.mp4     # Video file
    python main.py --no-flip             # Do not mirror webcam display

Keys:
    R: start/stop recording and reset scores when starting
    S: stop recording, show summary, and upload session to Firestore
    C: clear summary and continue
    Q or ESC: quit
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import Counter, deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Deque, Dict, Iterable, Optional, Tuple

import cv2
import mediapipe as mp
import numpy as np

try:
    import firebase_admin
    from firebase_admin import credentials, firestore
except ImportError:
    firebase_admin = None
    credentials = None
    firestore = None


mp_pose = mp.solutions.pose
mp_drawing = mp.solutions.drawing_utils
mp_drawing_styles = mp.solutions.drawing_styles


# Landmark indices are kept here for readability.
LM = mp_pose.PoseLandmark


@dataclass
class GaitFeatures:
    """Container for features displayed and used by the heuristic classifier."""

    left_knee_angle: float = float("nan")
    right_knee_angle: float = float("nan")
    left_hip_angle: float = float("nan")
    right_hip_angle: float = float("nan")
    step_length: float = float("nan")
    left_arm_swing: float = float("nan")
    right_arm_swing: float = float("nan")
    mean_arm_swing: float = float("nan")
    symmetry_index: float = float("nan")
    trunk_lean: float = float("nan")
    left_knee_lift: float = float("nan")
    right_knee_lift: float = float("nan")
    left_arm_close_to_chest: bool = False
    right_arm_close_to_chest: bool = False
    weak_side: str = "unknown"


@dataclass
class GaitPrediction:
    status: str
    color: Tuple[int, int, int]
    reasons: Tuple[str, ...] = ()


@dataclass
class SessionSummary:
    highest_risk: str
    risk_percentage: float
    total_frames: int
    risk_scores: Dict[str, int]
    upload_status: str
    document_id: Optional[str] = None


class GaitFeatureExtractor:
    """
    Extracts biometric gait metrics from MediaPipe landmarks.

    Distances are normalized by approximate body height in image coordinates.
    This makes the heuristic thresholds less dependent on camera resolution.
    """

    def __init__(self, history_size: int = 45, min_visibility: float = 0.6) -> None:
        self.history_size = history_size
        self.min_visibility = min_visibility
        self.left_wrist_rel_x: Deque[float] = deque(maxlen=history_size)
        self.right_wrist_rel_x: Deque[float] = deque(maxlen=history_size)
        self.left_ankle_x: Deque[float] = deque(maxlen=history_size)
        self.right_ankle_x: Deque[float] = deque(maxlen=history_size)
        self.left_knee_y: Deque[float] = deque(maxlen=history_size)
        self.right_knee_y: Deque[float] = deque(maxlen=history_size)

    def extract(
        self,
        landmarks: Iterable[mp.framework.formats.landmark_pb2.NormalizedLandmark],
        frame_shape: Tuple[int, int, int],
    ) -> Optional[GaitFeatures]:
        h, w = frame_shape[:2]
        points = self._landmark_dict(landmarks, w, h)

        required = (
            LM.LEFT_SHOULDER,
            LM.RIGHT_SHOULDER,
            LM.LEFT_HIP,
            LM.RIGHT_HIP,
            LM.LEFT_KNEE,
            LM.RIGHT_KNEE,
            LM.LEFT_ANKLE,
            LM.RIGHT_ANKLE,
            LM.LEFT_WRIST,
            LM.RIGHT_WRIST,
        )
        if not all(idx in points for idx in required):
            return None

        left_shoulder = points[LM.LEFT_SHOULDER]
        right_shoulder = points[LM.RIGHT_SHOULDER]
        left_hip = points[LM.LEFT_HIP]
        right_hip = points[LM.RIGHT_HIP]
        left_knee = points[LM.LEFT_KNEE]
        right_knee = points[LM.RIGHT_KNEE]
        left_ankle = points[LM.LEFT_ANKLE]
        right_ankle = points[LM.RIGHT_ANKLE]
        left_wrist = points[LM.LEFT_WRIST]
        right_wrist = points[LM.RIGHT_WRIST]

        body_height = self._estimate_body_height(points)
        if body_height <= 1.0:
            return None

        shoulder_mid = midpoint(left_shoulder, right_shoulder)
        hip_mid = midpoint(left_hip, right_hip)

        left_heel = points.get(LM.LEFT_HEEL, left_ankle)
        right_heel = points.get(LM.RIGHT_HEEL, right_ankle)

        left_knee_angle = angle_3d(left_hip, left_knee, left_ankle)
        right_knee_angle = angle_3d(right_hip, right_knee, right_ankle)
        left_hip_angle = angle_3d(left_shoulder, left_hip, left_knee)
        right_hip_angle = angle_3d(right_shoulder, right_hip, right_knee)
        step_length = distance_xz(left_heel, right_heel) / body_height
        trunk_lean = trunk_lean_angle_degrees(shoulder_mid, hip_mid)

        # Track short-window dynamics. X-axis movement is the most reliable for
        # a frontal webcam setup; z is noisier but still used in step length.
        self.left_wrist_rel_x.append((left_wrist[0] - left_hip[0]) / body_height)
        self.right_wrist_rel_x.append((right_wrist[0] - right_hip[0]) / body_height)
        self.left_ankle_x.append(left_ankle[0] / body_height)
        self.right_ankle_x.append(right_ankle[0] / body_height)
        self.left_knee_y.append(left_knee[1] / body_height)
        self.right_knee_y.append(right_knee[1] / body_height)

        left_arm_swing = range_or_nan(self.left_wrist_rel_x)
        right_arm_swing = range_or_nan(self.right_wrist_rel_x)
        mean_arm_swing = mean_nan(left_arm_swing, right_arm_swing)

        left_leg_swing = range_or_nan(self.left_ankle_x)
        right_leg_swing = range_or_nan(self.right_ankle_x)
        symmetry_index = symmetry_index_from_ranges(left_leg_swing, right_leg_swing)

        # Upward knee excursion across the rolling window. Higher values suggest
        # exaggerated hip/knee flexion during swing, as seen in steppage gait.
        left_knee_lift = range_or_nan(self.left_knee_y)
        right_knee_lift = range_or_nan(self.right_knee_y)

        left_arm_close = is_arm_held_close_to_chest(
            wrist=left_wrist,
            shoulder=left_shoulder,
            hip=left_hip,
            body_height=body_height,
            arm_swing=left_arm_swing,
        )
        right_arm_close = is_arm_held_close_to_chest(
            wrist=right_wrist,
            shoulder=right_shoulder,
            hip=right_hip,
            body_height=body_height,
            arm_swing=right_arm_swing,
        )
        weak_side = infer_weak_side(left_leg_swing, right_leg_swing)

        return GaitFeatures(
            left_knee_angle=left_knee_angle,
            right_knee_angle=right_knee_angle,
            left_hip_angle=left_hip_angle,
            right_hip_angle=right_hip_angle,
            step_length=step_length,
            left_arm_swing=left_arm_swing,
            right_arm_swing=right_arm_swing,
            mean_arm_swing=mean_arm_swing,
            symmetry_index=symmetry_index,
            trunk_lean=trunk_lean,
            left_knee_lift=left_knee_lift,
            right_knee_lift=right_knee_lift,
            left_arm_close_to_chest=left_arm_close,
            right_arm_close_to_chest=right_arm_close,
            weak_side=weak_side,
        )

    def _landmark_dict(
        self,
        landmarks: Iterable[mp.framework.formats.landmark_pb2.NormalizedLandmark],
        width: int,
        height: int,
    ) -> Dict[LM, np.ndarray]:
        points: Dict[LM, np.ndarray] = {}
        for idx, lm in enumerate(landmarks):
            landmark = LM(idx)
            if lm.visibility < self.min_visibility:
                continue
            # MediaPipe x/y are normalized to frame size. z is roughly in the
            # same scale as x, so scaling z by width keeps units comparable.
            points[landmark] = np.array([lm.x * width, lm.y * height, lm.z * width], dtype=np.float32)
        return points

    def _estimate_body_height(self, points: Dict[LM, np.ndarray]) -> float:
        left_shoulder = points.get(LM.LEFT_SHOULDER)
        right_shoulder = points.get(LM.RIGHT_SHOULDER)
        left_hip = points.get(LM.LEFT_HIP)
        right_hip = points.get(LM.RIGHT_HIP)
        left_ankle = points.get(LM.LEFT_ANKLE)
        right_ankle = points.get(LM.RIGHT_ANKLE)
        if any(p is None for p in (left_shoulder, right_shoulder, left_hip, right_hip, left_ankle, right_ankle)):
            return 0.0
        shoulder_mid = midpoint(left_shoulder, right_shoulder)
        hip_mid = midpoint(left_hip, right_hip)
        ankle_mid = midpoint(left_ankle, right_ankle)
        torso = np.linalg.norm(shoulder_mid[:2] - hip_mid[:2])
        lower_body = np.linalg.norm(hip_mid[:2] - ankle_mid[:2])
        return float(torso + lower_body)


class RuleBasedGaitClassifier:
    """
    Transparent MVP classifier.

    These thresholds are starting points only. For real clinical work, replace
    or tune them using labeled gait trials from the intended camera viewpoint.
    """

    PARKINSONIAN_ARM_SWING_MAX = 0.075
    PARKINSONIAN_STEP_LENGTH_MAX = 0.115
    PARKINSONIAN_TRUNK_LEAN_MIN = 10.0

    HEMIPLEGIC_SYMMETRY_MIN = 0.45
    HEMIPLEGIC_ARM_SWING_MAX = 0.08

    STEPPAGE_KNEE_LIFT_MIN = 0.105
    STEPPAGE_KNEE_FLEXION_MAX_ANGLE = 132.0

    def predict(self, features: Optional[GaitFeatures]) -> GaitPrediction:
        if features is None:
            return GaitPrediction("No Pose Detected", (0, 165, 255), ("Move fully into camera view",))

        reasons = []

        steppage = (
            max_nan(features.left_knee_lift, features.right_knee_lift) > self.STEPPAGE_KNEE_LIFT_MIN
            and min_nan(features.left_knee_angle, features.right_knee_angle) < self.STEPPAGE_KNEE_FLEXION_MAX_ANGLE
        )
        if steppage:
            side = "left" if features.left_knee_lift > features.right_knee_lift else "right"
            reasons.append(f"high {side} knee lift")
            reasons.append("excessive swing-phase knee flexion")
            return GaitPrediction("Possible Steppage Gait", (0, 140, 255), tuple(reasons))

        parkinsonian = (
            features.mean_arm_swing < self.PARKINSONIAN_ARM_SWING_MAX
            and features.trunk_lean > self.PARKINSONIAN_TRUNK_LEAN_MIN
            and features.step_length < self.PARKINSONIAN_STEP_LENGTH_MAX
        )
        if parkinsonian:
            reasons.append("reduced bilateral arm swing")
            reasons.append("short step length")
            reasons.append("forward trunk lean")
            return GaitPrediction("Possible Parkinsonian Gait", (0, 0, 255), tuple(reasons))

        one_arm_close = features.left_arm_close_to_chest or features.right_arm_close_to_chest
        low_arm_swing = min_nan(features.left_arm_swing, features.right_arm_swing) < self.HEMIPLEGIC_ARM_SWING_MAX
        hemiplegic = features.symmetry_index > self.HEMIPLEGIC_SYMMETRY_MIN and one_arm_close and low_arm_swing
        if hemiplegic:
            side = features.weak_side if features.weak_side != "unknown" else "one"
            reasons.append(f"{side} leg movement reduced/asymmetric")
            reasons.append("one arm held close to torso")
            return GaitPrediction("Possible Hemiplegic Gait", (0, 0, 255), tuple(reasons))

        return GaitPrediction("Normal / No Abnormal Pattern", (0, 180, 0), ("heuristics below alert thresholds",))


class FeatureSmoother:
    """
    Exponential moving-average filter applied to the scalar gait features.

    MediaPipe landmarks jitter frame-to-frame, and several features (joint
    angles, trunk lean, step length) are computed from a single instant. That
    jitter pushes min/max-based and threshold-based logic across decision
    boundaries, producing false positives. Smoothing each numeric feature in
    place stabilizes exactly what the classifier sees.

    alpha is the weight of the newest value: higher = more responsive but
    noisier, lower = smoother but laggier.
    """

    NUMERIC_FIELDS: Tuple[str, ...] = (
        "left_knee_angle",
        "right_knee_angle",
        "left_hip_angle",
        "right_hip_angle",
        "step_length",
        "left_arm_swing",
        "right_arm_swing",
        "mean_arm_swing",
        "symmetry_index",
        "trunk_lean",
        "left_knee_lift",
        "right_knee_lift",
    )

    def __init__(self, alpha: float = 0.4) -> None:
        self.alpha = alpha
        self.state: Dict[str, float] = {}

    def reset(self) -> None:
        self.state.clear()

    def smooth(self, features: Optional[GaitFeatures]) -> Optional[GaitFeatures]:
        # Lost pose: clear state so re-entry into frame starts fresh instead of
        # blending against stale values from a previous person/position.
        if features is None:
            self.reset()
            return None

        for field in self.NUMERIC_FIELDS:
            value = getattr(features, field)
            if not np.isfinite(value):
                continue
            previous = self.state.get(field)
            smoothed = value if previous is None else self.alpha * value + (1.0 - self.alpha) * previous
            self.state[field] = smoothed
            setattr(features, field, smoothed)
        return features


class PredictionSmoother:
    """
    Majority-vote filter over the most recent predictions.

    Per-frame classification flickers because it is computed from instantaneous,
    noisy features. Holding the displayed/recorded label to the majority status
    over a short window removes single-frame flips while still reacting within a
    fraction of a second.
    """

    def __init__(self, window: int = 9) -> None:
        self.history: Deque[GaitPrediction] = deque(maxlen=window)

    def reset(self) -> None:
        self.history.clear()

    def smooth(self, prediction: GaitPrediction) -> GaitPrediction:
        self.history.append(prediction)
        majority_status, _ = Counter(p.status for p in self.history).most_common(1)[0]
        # Return the most recent prediction carrying the majority status so the
        # reasons/color shown stay consistent with the chosen label.
        for past in reversed(self.history):
            if past.status == majority_status:
                return past
        return prediction


class GaitSessionRecorder:
    """Tracks frame-level gait classifications during a recording session."""

    LABELS = ("Normal", "Parkinsonian", "Hemiplegic", "Steppage")

    def __init__(self) -> None:
        self.is_recording = False
        self.total_frames = 0
        self.risk_scores: Dict[str, int] = {label: 0 for label in self.LABELS}

    def start(self) -> None:
        self.is_recording = True
        self.total_frames = 0
        self.risk_scores = {label: 0 for label in self.LABELS}

    def toggle(self) -> None:
        if self.is_recording:
            self.is_recording = False
        else:
            self.start()

    def stop(self) -> None:
        self.is_recording = False

    def record(self, prediction: GaitPrediction) -> None:
        if not self.is_recording:
            return
        label = normalize_prediction_label(prediction.status)
        if label not in self.risk_scores:
            label = "Normal"
        self.risk_scores[label] += 1
        self.total_frames += 1

    def result(self) -> Tuple[str, float]:
        """Return highest non-normal risk and percentage of recorded frames."""
        if self.total_frames <= 0:
            return "No Data", 0.0

        abnormal_counts = {label: count for label, count in self.risk_scores.items() if label != "Normal"}
        highest_risk, disease_frames = max(abnormal_counts.items(), key=lambda item: item[1])
        if disease_frames <= 0:
            highest_risk = "Normal"
            disease_frames = self.risk_scores.get("Normal", 0)

        risk_percentage = (disease_frames / self.total_frames) * 100.0
        return highest_risk, risk_percentage


class FirebaseGaitLogger:
    """Uploads gait session summaries to Cloud Firestore."""

    def __init__(self, service_account_path: Path, collection_name: str = "gait_assessments") -> None:
        self.service_account_path = service_account_path
        self.collection_name = collection_name
        self.db = None
        self.initialization_error: Optional[str] = None
        self._initialize()

    def _initialize(self) -> None:
        if firebase_admin is None or credentials is None or firestore is None:
            self.initialization_error = "firebase-admin is not installed"
            return

        if not self.service_account_path.exists():
            self.initialization_error = f"missing {self.service_account_path.name}"
            return

        try:
            if not firebase_admin._apps:
                cred = credentials.Certificate(str(self.service_account_path))
                firebase_admin.initialize_app(cred)
            self.db = firestore.client()
        except Exception as exc:
            self.initialization_error = f"Firebase init failed: {exc}"
            self.db = None

    @property
    def is_available(self) -> bool:
        return self.db is not None

    def upload(self, recorder: GaitSessionRecorder) -> Tuple[str, Optional[str]]:
        highest_risk, risk_percentage = recorder.result()
        payload = {
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "session_duration_frames": recorder.total_frames,
            "risk_scores": dict(recorder.risk_scores),
            "highest_risk_detected": {
                "condition": highest_risk,
                "confidence_risk_percentage": round(risk_percentage, 2),
            },
        }

        if not self.is_available:
            reason = self.initialization_error or "Firebase is unavailable"
            return f"Firebase upload skipped: {reason}", None

        try:
            _, doc_ref = self.db.collection(self.collection_name).add(payload)
            return "Successfully uploaded to Firebase!", doc_ref.id
        except Exception as exc:
            return f"Firebase upload failed: {exc}", None


def normalize_prediction_label(status: str) -> str:
    if "Parkinsonian" in status:
        return "Parkinsonian"
    if "Hemiplegic" in status:
        return "Hemiplegic"
    if "Steppage" in status:
        return "Steppage"
    return "Normal"


def angle_3d(a: np.ndarray, b: np.ndarray, c: np.ndarray) -> float:
    """Angle ABC in degrees for 3D points."""
    ba = a - b
    bc = c - b
    denom = np.linalg.norm(ba) * np.linalg.norm(bc)
    if denom < 1e-6:
        return float("nan")
    cosine = float(np.dot(ba, bc) / denom)
    cosine = np.clip(cosine, -1.0, 1.0)
    return float(np.degrees(np.arccos(cosine)))


def midpoint(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    return (a + b) / 2.0


def distance_xz(a: np.ndarray, b: np.ndarray) -> float:
    return float(np.linalg.norm(np.array([a[0] - b[0], a[2] - b[2]], dtype=np.float32)))


def trunk_lean_angle_degrees(shoulder_mid: np.ndarray, hip_mid: np.ndarray) -> float:
    """
    Estimate trunk lean angle relative to vertical.

    A frontal webcam cannot perfectly measure sagittal forward lean. This uses
    the image-plane shoulder-vs-hip displacement plus MediaPipe z displacement
    as a practical MVP estimate.
    """
    vertical = abs(float(shoulder_mid[1] - hip_mid[1]))
    horizontal_depth = math.sqrt(float((shoulder_mid[0] - hip_mid[0]) ** 2 + (shoulder_mid[2] - hip_mid[2]) ** 2))
    if vertical < 1e-6:
        return 0.0
    return float(np.degrees(np.arctan2(horizontal_depth, vertical)))


def range_or_nan(values: Deque[float]) -> float:
    if len(values) < max(5, values.maxlen // 5):
        return float("nan")
    arr = np.asarray(values, dtype=np.float32)
    return float(np.nanmax(arr) - np.nanmin(arr))


def symmetry_index_from_ranges(left_range: float, right_range: float) -> float:
    if not np.isfinite(left_range) or not np.isfinite(right_range):
        return float("nan")
    denominator = 0.5 * (abs(left_range) + abs(right_range)) + 1e-6
    return float(abs(left_range - right_range) / denominator)


def infer_weak_side(left_leg_swing: float, right_leg_swing: float) -> str:
    if not np.isfinite(left_leg_swing) or not np.isfinite(right_leg_swing):
        return "unknown"
    if abs(left_leg_swing - right_leg_swing) < 0.025:
        return "unknown"
    return "left" if left_leg_swing < right_leg_swing else "right"


def is_arm_held_close_to_chest(
    wrist: np.ndarray,
    shoulder: np.ndarray,
    hip: np.ndarray,
    body_height: float,
    arm_swing: float,
) -> bool:
    torso_mid = midpoint(shoulder, hip)
    wrist_to_torso = float(np.linalg.norm((wrist[:2] - torso_mid[:2]) / body_height))
    wrist_between_shoulder_and_hip = min(shoulder[1], hip[1]) <= wrist[1] <= max(shoulder[1], hip[1]) + 0.12 * body_height
    reduced_swing = np.isfinite(arm_swing) and arm_swing < RuleBasedGaitClassifier.HEMIPLEGIC_ARM_SWING_MAX
    return wrist_to_torso < 0.28 and wrist_between_shoulder_and_hip and reduced_swing


def min_nan(a: float, b: float) -> float:
    vals = [v for v in (a, b) if np.isfinite(v)]
    return min(vals) if vals else float("nan")


def max_nan(a: float, b: float) -> float:
    vals = [v for v in (a, b) if np.isfinite(v)]
    return max(vals) if vals else float("nan")


def mean_nan(a: float, b: float) -> float:
    vals = [v for v in (a, b) if np.isfinite(v)]
    return float(sum(vals) / len(vals)) if vals else float("nan")


def fmt(value: float, suffix: str = "", precision: int = 1) -> str:
    if not np.isfinite(value):
        return "--"
    return f"{value:.{precision}f}{suffix}"


def draw_status_panel(
    frame: np.ndarray,
    features: Optional[GaitFeatures],
    prediction: GaitPrediction,
    recorder: Optional[GaitSessionRecorder] = None,
) -> None:
    h, w = frame.shape[:2]
    panel_w = min(540, w - 20)
    panel_h = 255
    x0, y0 = 10, 10

    overlay = frame.copy()
    cv2.rectangle(overlay, (x0, y0), (x0 + panel_w, y0 + panel_h), (18, 18, 18), -1)
    cv2.addWeighted(overlay, 0.72, frame, 0.28, 0, frame)
    cv2.rectangle(frame, (x0, y0), (x0 + panel_w, y0 + panel_h), prediction.color, 2)

    cv2.putText(
        frame,
        prediction.status,
        (x0 + 15, y0 + 38),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.82,
        prediction.color,
        2,
        cv2.LINE_AA,
    )
    cv2.putText(
        frame,
        "MVP screening only - not diagnostic",
        (x0 + 15, y0 + 65),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.48,
        (210, 210, 210),
        1,
        cv2.LINE_AA,
    )

    if features is None:
        lines = [
            "Pose: waiting for full body landmarks",
            "Tip: use a stable, full-body side or front view",
        ]
    else:
        lines = [
            f"Knee angles L/R: {fmt(features.left_knee_angle, ' deg')} / {fmt(features.right_knee_angle, ' deg')}",
            f"Hip angles   L/R: {fmt(features.left_hip_angle, ' deg')} / {fmt(features.right_hip_angle, ' deg')}",
            f"Step length: {fmt(features.step_length, precision=3)} body-heights",
            f"Arm swing L/R: {fmt(features.left_arm_swing, precision=3)} / {fmt(features.right_arm_swing, precision=3)}",
            f"Symmetry index: {fmt(features.symmetry_index, precision=3)}   Weak side: {features.weak_side}",
            f"Trunk lean: {fmt(features.trunk_lean, ' deg')}   Knee lift L/R: {fmt(features.left_knee_lift, precision=3)} / {fmt(features.right_knee_lift, precision=3)}",
        ]

    y = y0 + 100
    for line in lines:
        cv2.putText(frame, line, (x0 + 15, y), cv2.FONT_HERSHEY_SIMPLEX, 0.52, (235, 235, 235), 1, cv2.LINE_AA)
        y += 25

    reason_text = " | ".join(prediction.reasons[:2])
    if reason_text:
        cv2.putText(
            frame,
            reason_text,
            (x0 + 15, min(y0 + panel_h - 15, y + 7)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (200, 220, 255),
            1,
            cv2.LINE_AA,
        )

    if recorder is not None:
        draw_recording_status(frame, recorder)


def draw_recording_status(frame: np.ndarray, recorder: GaitSessionRecorder) -> None:
    h, w = frame.shape[:2]
    color = (0, 0, 255) if recorder.is_recording else (120, 120, 120)
    label = "REC" if recorder.is_recording else "IDLE"
    x0 = max(10, w - 240)
    y0 = 18

    cv2.rectangle(frame, (x0, y0), (w - 10, y0 + 88), (18, 18, 18), -1)
    cv2.rectangle(frame, (x0, y0), (w - 10, y0 + 88), color, 2)
    cv2.circle(frame, (x0 + 24, y0 + 24), 8, color, -1)
    cv2.putText(frame, label, (x0 + 42, y0 + 32), cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv2.LINE_AA)
    cv2.putText(
        frame,
        f"Frames: {recorder.total_frames}",
        (x0 + 16, y0 + 58),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.5,
        (235, 235, 235),
        1,
        cv2.LINE_AA,
    )
    cv2.putText(
        frame,
        "R record  S summary  Q quit",
        (x0 + 16, y0 + 79),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.43,
        (205, 205, 205),
        1,
        cv2.LINE_AA,
    )


def build_session_summary(recorder: GaitSessionRecorder, logger: FirebaseGaitLogger) -> SessionSummary:
    highest_risk, risk_percentage = recorder.result()
    upload_status, document_id = logger.upload(recorder)
    return SessionSummary(
        highest_risk=highest_risk,
        risk_percentage=risk_percentage,
        total_frames=recorder.total_frames,
        risk_scores=dict(recorder.risk_scores),
        upload_status=upload_status,
        document_id=document_id,
    )


def draw_summary_modal(frame: np.ndarray, summary: SessionSummary) -> np.ndarray:
    output = frame.copy()
    h, w = output.shape[:2]

    # Dim the full frame, then draw a stronger centered modal panel.
    dim = output.copy()
    cv2.rectangle(dim, (0, 0), (w, h), (0, 0, 0), -1)
    cv2.addWeighted(dim, 0.45, output, 0.55, 0, output)

    modal_w = min(760, int(w * 0.88))
    modal_h = min(420, int(h * 0.78))
    x0 = (w - modal_w) // 2
    y0 = (h - modal_h) // 2
    x1 = x0 + modal_w
    y1 = y0 + modal_h

    panel = output.copy()
    cv2.rectangle(panel, (x0, y0), (x1, y1), (16, 16, 20), -1)
    cv2.addWeighted(panel, 0.88, output, 0.12, 0, output)
    cv2.rectangle(output, (x0, y0), (x1, y1), (80, 170, 255), 2)

    title_y = y0 + 58
    cv2.putText(
        output,
        "Gait Analysis Summary",
        (x0 + 34, title_y),
        cv2.FONT_HERSHEY_SIMPLEX,
        1.05,
        (255, 255, 255),
        2,
        cv2.LINE_AA,
    )

    lines = [
        f"Highest Risk: {summary.highest_risk} ({summary.risk_percentage:.1f}%)",
        f"Total Frames Evaluated: {summary.total_frames}",
        f"Normal: {summary.risk_scores.get('Normal', 0)}",
        f"Parkinsonian: {summary.risk_scores.get('Parkinsonian', 0)}",
        f"Hemiplegic: {summary.risk_scores.get('Hemiplegic', 0)}",
        f"Steppage: {summary.risk_scores.get('Steppage', 0)}",
        f"Status: {summary.upload_status}",
    ]

    y = y0 + 110
    for line in lines:
        color = (220, 245, 220) if line.startswith("Status: Successfully") else (230, 230, 235)
        cv2.putText(output, line, (x0 + 38, y), cv2.FONT_HERSHEY_SIMPLEX, 0.66, color, 1, cv2.LINE_AA)
        y += 34

    if summary.document_id:
        cv2.putText(
            output,
            f"Firestore Document ID: {summary.document_id}",
            (x0 + 38, min(y, y1 - 58)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.54,
            (190, 215, 255),
            1,
            cv2.LINE_AA,
        )

    cv2.putText(
        output,
        "Press 'C' to clear summary and continue",
        (x0 + 38, y1 - 24),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.62,
        (80, 210, 255),
        2,
        cv2.LINE_AA,
    )
    return output


def parse_source(source: str) -> int | str:
    if source.isdigit():
        return int(source)
    return source


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Real-time clinical gait analysis MVP with OpenCV and MediaPipe.")
    parser.add_argument("--source", default="0", help="Camera index or video path. Default: 0")
    parser.add_argument(
        "--firebase-key",
        default="serviceAccountKey.json",
        help="Path to Firebase service account JSON. Default: serviceAccountKey.json",
    )
    parser.add_argument("--history", type=int, default=45, help="Rolling window size for motion features.")
    parser.add_argument("--min-visibility", type=float, default=0.6, help="Minimum landmark visibility to use a point.")
    parser.add_argument(
        "--feature-smoothing",
        type=float,
        default=0.4,
        help="EMA weight for new feature values (0-1). Lower = smoother, laggier. Default: 0.4",
    )
    parser.add_argument(
        "--vote-window",
        type=int,
        default=9,
        help="Frames of majority voting applied to predictions before display. Default: 9",
    )
    parser.add_argument("--model-complexity", type=int, default=1, choices=(0, 1, 2), help="MediaPipe pose model complexity.")
    parser.add_argument("--min-detection-confidence", type=float, default=0.5, help="MediaPipe detection confidence.")
    parser.add_argument("--min-tracking-confidence", type=float, default=0.5, help="MediaPipe tracking confidence.")
    parser.add_argument("--resize-width", type=int, default=960, help="Resize display/processing width. Use 0 to keep original.")
    parser.add_argument("--no-flip", action="store_true", help="Do not mirror webcam input.")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    source = parse_source(args.source)
    is_camera = isinstance(source, int)

    cap = cv2.VideoCapture(source)
    if not cap.isOpened():
        print(f"ERROR: Could not open video source: {args.source}", file=sys.stderr)
        return 1

    extractor = GaitFeatureExtractor(history_size=args.history, min_visibility=args.min_visibility)
    classifier = RuleBasedGaitClassifier()
    feature_smoother = FeatureSmoother(alpha=args.feature_smoothing)
    prediction_smoother = PredictionSmoother(window=args.vote_window)
    recorder = GaitSessionRecorder()
    firebase_logger = FirebaseGaitLogger(Path(args.firebase_key))
    window_name = "Real-Time Clinical Gait Analysis MVP"
    summary_frame: Optional[np.ndarray] = None

    with mp_pose.Pose(
        static_image_mode=False,
        model_complexity=args.model_complexity,
        smooth_landmarks=True,
        enable_segmentation=False,
        min_detection_confidence=args.min_detection_confidence,
        min_tracking_confidence=args.min_tracking_confidence,
    ) as pose:
        while True:
            if summary_frame is not None:
                cv2.imshow(window_name, summary_frame)
                key = cv2.waitKey(30) & 0xFF
                if key in (ord("c"), ord("C")):
                    summary_frame = None
                elif key in (27, ord("q"), ord("Q")):
                    break
                continue

            ok, frame = cap.read()
            if not ok:
                print("End of stream or unable to read frame.")
                break

            if args.resize_width and frame.shape[1] > 0:
                scale = args.resize_width / frame.shape[1]
                frame = cv2.resize(frame, (args.resize_width, int(frame.shape[0] * scale)), interpolation=cv2.INTER_AREA)

            if is_camera and not args.no_flip:
                frame = cv2.flip(frame, 1)

            rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            rgb.flags.writeable = False
            result = pose.process(rgb)
            rgb.flags.writeable = True

            features = None
            if result.pose_landmarks:
                features = extractor.extract(result.pose_landmarks.landmark, frame.shape)
                mp_drawing.draw_landmarks(
                    frame,
                    result.pose_landmarks,
                    mp_pose.POSE_CONNECTIONS,
                    landmark_drawing_spec=mp_drawing_styles.get_default_pose_landmarks_style(),
                )

            features = feature_smoother.smooth(features)
            prediction = prediction_smoother.smooth(classifier.predict(features))
            recorder.record(prediction)
            draw_status_panel(frame, features, prediction, recorder)

            cv2.imshow(window_name, frame)
            key = cv2.waitKey(1 if is_camera else 20) & 0xFF
            if key in (27, ord("q"), ord("Q")):
                break
            if key in (ord("r"), ord("R")):
                recorder.toggle()
                if recorder.is_recording:
                    feature_smoother.reset()
                    prediction_smoother.reset()
            elif key in (ord("s"), ord("S")):
                recorder.stop()
                summary = build_session_summary(recorder, firebase_logger)
                summary_frame = draw_summary_modal(frame, summary)

    cap.release()
    cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
