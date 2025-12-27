#pragma once

#include <math.h>
#include <stdint.h>
#include <deque>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// =======================
// Basic Math Structures
// =======================
struct Vec3 {
    float x, y, z;
    Vec3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vec3 operator+(const Vec3& other) const { return Vec3(x + other.x, y + other.y, z + other.z); }
    Vec3 operator-(const Vec3& other) const { return Vec3(x - other.x, y - other.y, z - other.z); }
    Vec3 operator*(float scalar) const { return Vec3(x * scalar, y * scalar, z * scalar); }

    float norm() const { return sqrtf(x*x + y*y + z*z); }
    float normSquared() const { return x*x + y*y + z*z; }

    void normalize() {
        float n = norm();
        if (n > 1e-6f) { x /= n; y /= n; z /= n; }
    }

    static float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

    static Vec3 cross(const Vec3& a, const Vec3& b) {
        return Vec3(
            a.y*b.z - a.z*b.y,
            a.z*b.x - a.x*b.z,
            a.x*b.y - a.y*b.x
        );
    }
};

struct Quaternion {
    float w, x, y, z;
    Quaternion() : w(1.0f), x(0.0f), y(0.0f), z(0.0f) {}
    Quaternion(float _w, float _x, float _y, float _z) : w(_w), x(_x), y(_y), z(_z) {}
    static Quaternion identity() { return Quaternion(1.0f, 0.0f, 0.0f, 0.0f); }
};

// =======================
// Quaternion Operations
// =======================
static inline Quaternion quatConjugate(const Quaternion &q) {
    return Quaternion(q.w, -q.x, -q.y, -q.z);
}

static inline Quaternion quatMultiply(const Quaternion &q1, const Quaternion &q2) {
    return Quaternion(
        q1.w*q2.w - q1.x*q2.x - q1.y*q2.y - q1.z*q2.z,
        q1.w*q2.x + q1.x*q2.w + q1.y*q2.z - q1.z*q2.y,
        q1.w*q2.y - q1.x*q2.z + q1.y*q2.w + q1.z*q2.x,
        q1.w*q2.z + q1.x*q2.y - q1.y*q2.x + q1.z*q2.w
    );
}

// g_world = [0,0,1], so g_body = R(q) * [0,0,1]
static inline Vec3 quatRotateGravity(const Quaternion &q) {
    return Vec3(
        2.0f * (q.x * q.z - q.w * q.y),
        2.0f * (q.w * q.x + q.y * q.z),
        q.w*q.w - q.x*q.x - q.y*q.y + q.z*q.z
    );
}

static inline void quatNormalize(Quaternion &q) {
    float n = sqrtf(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
    if (n > 1e-6f) { q.w /= n; q.x /= n; q.y /= n; q.z /= n; }
    else { q = Quaternion::identity(); }
}

// =======================
// SHOE Detector (Skog et al., Eq. 21)
// =======================
class SHOEDetector {
public:
    SHOEDetector() {
        sigma_a = 0.02f;                     // m/s^2
        sigma_w = 0.1f * (M_PI / 180.0f);     // 0.1 deg/s -> rad/s
        windowSize = 5;                      // N = 5
        reset();
    }

    void reset() { gyroBuffer.clear(); accBuffer.clear(); }

    float computeTestStatistic() {
        if (gyroBuffer.size() != (size_t)windowSize) return 1e6f;

        Vec3 avg_acc(0,0,0);
        for (const auto& a : accBuffer) avg_acc = avg_acc + a;
        avg_acc = avg_acc * (1.0f / windowSize);

        float mag_avg = avg_acc.norm();
        if (mag_avg < 1e-6f) return 1e6f;

        Vec3 u_hat(avg_acc.x / mag_avg, avg_acc.y / mag_avg, avg_acc.z / mag_avg);

        float acc_error_sum = 0.0f;
        float gyro_energy_sum = 0.0f;
        const float g = 9.81f;

        for (size_t i = 0; i < (size_t)windowSize; ++i) {
            Vec3 diff = accBuffer[i] - u_hat * g;
            acc_error_sum += diff.normSquared();
            gyro_energy_sum += gyroBuffer[i].normSquared();
        }

        float T = (1.0f / windowSize) * (
            (1.0f / (sigma_a * sigma_a)) * acc_error_sum +
            (1.0f / (sigma_w * sigma_w)) * gyro_energy_sum
        );

        return T;
    }

    bool update(const Vec3& gyro, const Vec3& acc, float threshold = 100.0f) {
        gyroBuffer.push_back(gyro);
        accBuffer.push_back(acc);

        if ((int)gyroBuffer.size() > windowSize) { gyroBuffer.pop_front(); accBuffer.pop_front(); }
        if ((int)gyroBuffer.size() < windowSize) return false;

        float T = computeTestStatistic();
        return T < threshold;
    }

    void setNoiseParams(float sigma_acc, float sigma_gyro_deg_per_sec) {
        sigma_a = sigma_acc;
        sigma_w = sigma_gyro_deg_per_sec * (M_PI / 180.0f);
    }

    void setWindowSize(int N) { windowSize = N; }

private:
    std::deque<Vec3> gyroBuffer;
    std::deque<Vec3> accBuffer;
    int windowSize;
    float sigma_a;
    float sigma_w;
};

// =======================
// Complementary Filter
// =======================
class ComplementaryFilter {
public:
    ComplementaryFilter() { reset(); }

    void reset() { q = Quaternion::identity(); beta = 0.03f; }

    void setQuaternion(const Quaternion& qin) { q = qin; quatNormalize(q); }
    const Quaternion& getQuaternion() const { return q; }
    void setBeta(float b) { beta = b; }

    void update(const Vec3& gyro, const Vec3& acc, float dt) {
        if (dt <= 0.0f) dt = 0.01f;

        Vec3 a = acc;
        float a_norm = a.norm();
        if (a_norm > 1e-6f) { a.x /= a_norm; a.y /= a_norm; a.z /= a_norm; }

        Vec3 g_est = quatRotateGravity(q);
        Vec3 error = Vec3::cross(a, g_est);

        Vec3 omega_corrected(
            gyro.x - beta * error.x,
            gyro.y - beta * error.y,
            gyro.z - beta * error.z
        );

        Quaternion q_dot;
        q_dot.w = -0.5f * (q.x * omega_corrected.x + q.y * omega_corrected.y + q.z * omega_corrected.z);
        q_dot.x =  0.5f * (q.w * omega_corrected.x + q.y * omega_corrected.z - q.z * omega_corrected.y);
        q_dot.y =  0.5f * (q.w * omega_corrected.y - q.x * omega_corrected.z + q.z * omega_corrected.x);
        q_dot.z =  0.5f * (q.w * omega_corrected.z + q.x * omega_corrected.y - q.y * omega_corrected.x);

        q.w += q_dot.w * dt;
        q.x += q_dot.x * dt;
        q.y += q_dot.y * dt;
        q.z += q_dot.z * dt;

        quatNormalize(q);
    }

    void applyZUPT(const Vec3& acc) {
        Vec3 a = acc; a.normalize();

        float roll  = atan2f(a.y, a.z);
        float pitch = atan2f(-a.x, sqrtf(a.y*a.y + a.z*a.z));

        float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
        float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);

        q.w = cp * cr;
        q.x = cp * sr;
        q.y = sp * cr;
        q.z = -sp * sr;

        quatNormalize(q);
    }

private:
    Quaternion q;
    float beta;
};

// =======================
// Dual-IMU Monitor (optional)
// =======================
class LumbarMonitor {
public:
    LumbarMonitor() { reset(); }

    void reset() {
        filter_pelvis.reset();
        filter_lumbar.reset();
        detector.reset();
        lumbar_angle_rad = 0.0f;
        zupt_threshold = 50.0f;
    }

    static float quatToPitch(const Quaternion& q) {
        float sinp = 2.0f * (q.w * q.y - q.z * q.x);
        if (sinp >= 1.0f) return  M_PI / 2.0f;
        if (sinp <= -1.0f) return -M_PI / 2.0f;
        return asinf(sinp);
    }

    static float calculateLumbarAngle(const Quaternion& q_pelvis, const Quaternion& q_lumbar) {
        Quaternion q_rel = quatMultiply(quatConjugate(q_pelvis), q_lumbar);
        return quatToPitch(q_rel);
    }

    void update(
        const Vec3& gyro_p, const Vec3& acc_p,
        const Vec3& gyro_l, const Vec3& acc_l,
        float dt
    ) {
        bool stationary_p = detector.update(gyro_p, acc_p, zupt_threshold);
        bool stationary_l = detector.update(gyro_l, acc_l, zupt_threshold);
        bool stationary = stationary_p && stationary_l;

        if (stationary) {
            filter_pelvis.applyZUPT(acc_p);
            filter_lumbar.applyZUPT(acc_l);
        } else {
            filter_pelvis.update(gyro_p, acc_p, dt);
            filter_lumbar.update(gyro_l, acc_l, dt);
        }

        lumbar_angle_rad = calculateLumbarAngle(
            filter_pelvis.getQuaternion(),
            filter_lumbar.getQuaternion()
        );
    }

    float getLumbarAngleDeg() const { return lumbar_angle_rad * 180.0f / M_PI; }
    void setZUPTThreshold(float T) { zupt_threshold = T; }

private:
    ComplementaryFilter filter_pelvis;
    ComplementaryFilter filter_lumbar;
    SHOEDetector detector;
    float lumbar_angle_rad;
    float zupt_threshold;
};
