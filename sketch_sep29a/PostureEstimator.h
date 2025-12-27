#pragma once

#include <Arduino.h>
#include <math.h>
#include "RobustOrientationFilter.h"

// ============================================================================
// Upright Calibration:
//  - At startup calibration posture is UPRIGHT (pelvis and lumbar aligned).
//  - Store initial relative pitch as zeroOffsetDeg.
//
// Real observed direction (your current hardware):
//  - Lumbar forward  -> angle DECREASES  (delta negative)  -> FLEXION
//  - Lumbar backward -> angle INCREASES  (delta positive)  -> EXTENSION (toward upright)
//
// 3 cases:
//  1) FLEXION   : angle decreases large & fast
//  2) EXTENSION : angle increases large & fast
//  3) UPRIGHT   : near zero + small/slow hold -> force angle=0
//
// Zone for haptics uses |angle| thresholds.
// ============================================================================
class PostureEstimator {
public:
    enum PostureZone { ZONE_SAFE, ZONE_WARNING, ZONE_CRITICAL };
    enum MovementCase { CASE_UPRIGHT, CASE_FLEXION, CASE_EXTENSION };

    PostureEstimator() {
        // zone thresholds (deg) based on |angle|
        warningThresholdDeg  = 20.0f;
        criticalThresholdDeg = 30.0f;

        // "large & fast" thresholds
        largeDeltaDeg = 2.0f;     // deg per update
        largeRateDegS = 60.0f;    // deg/s

        // upright snap thresholds
        uprightAngleDeg = 4.0f;
        smallDeltaDeg   = 0.25f;
        smallRateDegS   = 8.0f;
        uprightHoldMs   = 250;

        currentAngleDeg = 0.0f;
        currentZone = ZONE_SAFE;
        currentCase = CASE_UPRIGHT;

        zeroOffsetDeg = 0.0f;
        referenceSet  = false;

        prevAngleDeg = 0.0f;
        prevUpdateMs = 0;
        uprightStableMs = 0;

        lastToggle = 0;
        motorState = false;
    }

    void setUprightReference(const Quaternion& qPelvis, const Quaternion& qLumbar) {
        const float rawSignedDeg = computeRawSignedAngleDeg(qPelvis, qLumbar);
        zeroOffsetDeg = rawSignedDeg;
        referenceSet  = true;

        prevAngleDeg = 0.0f;
        prevUpdateMs = 0;
        uprightStableMs = 0;

        currentAngleDeg = 0.0f;
        currentCase = CASE_UPRIGHT;
        setZoneFromAngle();
    }

    bool  isReferenceSet() const { return referenceSet; }
    float getZeroOffsetDeg() const { return zeroOffsetDeg; }

    void update(const Quaternion& qPelvis, const Quaternion& qLumbar) {
        if (!referenceSet) {
            setUprightReference(qPelvis, qLumbar);
            return;
        }

        const unsigned long nowMs = millis();
        const bool firstSample = (prevUpdateMs == 0);

        float dt = 0.01f;
        if (!firstSample) {
            unsigned long dms = nowMs - prevUpdateMs;
            if (dms < 1)   dms = 1;
            if (dms > 100) dms = 100;
            dt = (float)dms * 1e-3f;
        }
        prevUpdateMs = nowMs;

        const float rawSignedDeg = computeRawSignedAngleDeg(qPelvis, qLumbar);
        const float signedDeg = rawSignedDeg - zeroOffsetDeg; // upright -> 0

        if (firstSample) {
            prevAngleDeg = signedDeg;

            if (fabsf(signedDeg) <= uprightAngleDeg) {
                currentCase = CASE_UPRIGHT;
                currentAngleDeg = 0.0f;
            } else {
                currentAngleDeg = signedDeg;
                // optional initial guess based on sign:
                // with your real direction: forward->more negative => flexion likely negative
                currentCase = (signedDeg < 0.0f) ? CASE_FLEXION : CASE_EXTENSION;
            }

            setZoneFromAngle();
            return;
        }

        const float deltaDeg = signedDeg - prevAngleDeg;
        const float rateDegS = deltaDeg / (dt > 1e-6f ? dt : 0.01f);
        prevAngleDeg = signedDeg;

        // ======================================================
        // THREE CASES (REVERSED as you requested)
        //
        // FLEXION   : angle DECREASES large & fast   (delta -, rate -)
        // EXTENSION : angle INCREASES large & fast   (delta +, rate +)
        // UPRIGHT   : near 0 + small/slow hold       -> angle = 0
        // ======================================================
        if (deltaDeg <= -largeDeltaDeg && rateDegS <= -largeRateDegS) {
            currentCase = CASE_FLEXION;
            uprightStableMs = 0;
        }
        else if (deltaDeg >= largeDeltaDeg && rateDegS >= largeRateDegS) {
            currentCase = CASE_EXTENSION;
            uprightStableMs = 0;
        }
        else {
            const bool nearUpright = (fabsf(signedDeg) <= uprightAngleDeg);
            const bool smallChange = (fabsf(deltaDeg) <= smallDeltaDeg) && (fabsf(rateDegS) <= smallRateDegS);

            if (nearUpright && smallChange) {
                if (uprightStableMs == 0) uprightStableMs = nowMs;
                if ((nowMs - uprightStableMs) >= uprightHoldMs) {
                    currentCase = CASE_UPRIGHT;
                }
            } else {
                uprightStableMs = 0;

                // avoid flicker after upright:
                // With reversed mapping:
                //   rate < 0 => flexion, rate > 0 => extension
                if (currentCase == CASE_UPRIGHT && rateDegS < 0.0f) currentCase = CASE_FLEXION;
                if (currentCase == CASE_UPRIGHT && rateDegS > 0.0f) currentCase = CASE_EXTENSION;
            }
        }

        currentAngleDeg = (currentCase == CASE_UPRIGHT) ? 0.0f : signedDeg;
        setZoneFromAngle();
    }

    float getAngle() const { return currentAngleDeg; }

    String getZoneString() const {
        switch (currentZone) {
            case ZONE_SAFE:     return "GREEN";
            case ZONE_WARNING:  return "YELLOW";
            case ZONE_CRITICAL: return "RED";
            default:            return "UNKNOWN";
        }
    }

    String getCaseString() const {
        switch (currentCase) {
            case CASE_FLEXION:   return "FLEXION";
            case CASE_EXTENSION: return "EXTENSION";
            case CASE_UPRIGHT:   return "UPRIGHT";
            default:             return "UNKNOWN";
        }
    }

    // old compatibility: zone only
    String getStateString() const { return getZoneString(); }

    // combined "GREEN|FLEXION"
    String getCombinedStateString() const {
        return getZoneString() + String("|") + getCaseString();
    }

    void handleHapticFeedback(int motorPin) {
        unsigned long now = millis();

        if (currentZone == ZONE_SAFE) {
            if (motorState) {
                digitalWrite(motorPin, LOW);
                motorState = false;
            }
            return;
        }

        if (currentZone == ZONE_CRITICAL) {
            digitalWrite(motorPin, HIGH);
            motorState = true;
            return;
        }

        // WARNING pulse
        if (now - lastToggle > 200) {
            motorState = !motorState;
            digitalWrite(motorPin, motorState ? HIGH : LOW);
            lastToggle = now;
        }
    }

    void setZoneThresholds(float warningDeg, float criticalDeg) {
        warningThresholdDeg = warningDeg;
        criticalThresholdDeg = criticalDeg;
    }

    void setMotionThresholds(
        float largeDelta_deg,
        float largeRate_deg_s,
        float uprightAngle_deg,
        float smallDelta_deg,
        float smallRate_deg_s,
        unsigned long uprightHold_ms
    ) {
        largeDeltaDeg = largeDelta_deg;
        largeRateDegS = largeRate_deg_s;
        uprightAngleDeg = uprightAngle_deg;
        smallDeltaDeg = smallDelta_deg;
        smallRateDegS = smallRate_deg_s;
        uprightHoldMs = uprightHold_ms;
    }

private:
    static float computeRawSignedAngleDeg(const Quaternion& qPelvis, const Quaternion& qLumbar) {
        Quaternion qRel = quatMultiply(quatConjugate(qPelvis), qLumbar);
        quatNormalize(qRel);

        float term = 2.0f * (qRel.w * qRel.y - qRel.z * qRel.x);
        if (term >  1.0f) term =  1.0f;
        if (term < -1.0f) term = -1.0f;

        float angleRad = asinf(term);
        return angleRad * (180.0f / M_PI);
    }

    void setZoneFromAngle() {
        float mag = fabsf(currentAngleDeg);
        if (mag < warningThresholdDeg) currentZone = ZONE_SAFE;
        else if (mag < criticalThresholdDeg) currentZone = ZONE_WARNING;
        else currentZone = ZONE_CRITICAL;
    }

private:
    float warningThresholdDeg, criticalThresholdDeg;

    float largeDeltaDeg, largeRateDegS;
    float uprightAngleDeg, smallDeltaDeg, smallRateDegS;
    unsigned long uprightHoldMs;

    float currentAngleDeg;
    PostureZone currentZone;
    MovementCase currentCase;

    float zeroOffsetDeg;
    bool  referenceSet;

    float prevAngleDeg;
    unsigned long prevUpdateMs;
    unsigned long uprightStableMs;

    unsigned long lastToggle;
    bool motorState;
};
