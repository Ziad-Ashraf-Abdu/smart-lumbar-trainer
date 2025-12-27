// PROJECT: Dual-IMU Smart Trainer
// HARDWARE: ESP32-WROOM-32E + 2x MPU6050
// =============================================================

#include <Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "mbedtls/aes.h"

#include "RobustOrientationFilter.h"
#include "PostureEstimator.h"

// ================= HARDWARE CONFIG =================
#define LED_PIN 2
#define HAPTIC_PIN 4

// Pelvis (reference)
#define SDA_PELVIS 21
#define SCL_PELVIS 22
const uint8_t ADDR_PELVIS = 0x68;

// Lumbar (moving)
#define SDA_LUMBAR 32
#define SCL_LUMBAR 33
const uint8_t ADDR_LUMBAR = 0x69;

// MPU Registers
#define MPU_PWR_MGMT_1   0x6B
#define MPU_CONFIG       0x1A
#define MPU_GYRO_CONFIG  0x1B
#define MPU_ACCEL_CONFIG 0x1C
#define MPU_ACCEL_XOUT_H 0x3B

// ================= BLE CONFIG =================
#define DEVICE_NAME     "ESP32_Smart_Spine"
#define ACTIVATION_KEY  "LBPP-DEMO-KEY-2024"
#define SERVICE_UUID    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_CHAR_UUID    "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_CHAR_UUID    "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// ================= OBJECTS =================
ComplementaryFilter filterPelvis;
ComplementaryFilter filterLumbar;

SHOEDetector detectorPelvis;
SHOEDetector detectorLumbar;

PostureEstimator estimator;

Vec3 biasPelvis(0,0,0);
Vec3 biasLumbar(0,0,0);

float ZUPT_THRESHOLD = 50.0f;
bool isCalibrated = false;

// BLE Globals
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool authenticated = false;

mbedtls_aes_context aes;
unsigned char key[16];
unsigned char iv[16];

const size_t MAX_PACKET_LEN = 64;
unsigned long lastMicros = 0;

// ================= HELPER: QUAT -> EULER =================
void quatToEuler(const Quaternion& q, float& roll, float& pitch, float& yaw) {
  float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
  float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
  roll = atan2f(sinr_cosp, cosr_cosp);

  float sinp = 2.0f * (q.w * q.y - q.z * q.x);
  if (fabsf(sinp) >= 1.0f) pitch = copysignf(M_PI / 2.0f, sinp);
  else pitch = asinf(sinp);

  float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
  float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
  yaw = atan2f(siny_cosp, cosy_cosp);
}

// ================= I2C DRIVERS =================
void writeRegister(TwoWire &bus, uint8_t address, uint8_t reg, uint8_t data) {
  bus.beginTransmission(address);
  bus.write(reg);
  bus.write(data);
  bus.endTransmission();
}

bool initMPU(TwoWire &bus, uint8_t address, const char* name) {
  Serial.print("Init "); Serial.print(name); Serial.print("... ");
  bus.beginTransmission(address);
  if (bus.endTransmission() != 0) {
    Serial.println("FAILED - Check wiring!");
    return false;
  }
  writeRegister(bus, address, MPU_PWR_MGMT_1, 0x00);
  delay(10);
  writeRegister(bus, address, MPU_GYRO_CONFIG, 0x08);
  writeRegister(bus, address, MPU_ACCEL_CONFIG, 0x10);
  writeRegister(bus, address, MPU_CONFIG, 0x04);
  Serial.println("OK");
  return true;
}

bool readMPU(TwoWire &bus, uint8_t address, Vec3 &acc, Vec3 &gyro) {
  bus.beginTransmission(address);
  bus.write(MPU_ACCEL_XOUT_H);
  if (bus.endTransmission(false) != 0) return false;
  if (bus.requestFrom(address, (uint8_t)14) < 14) return false;

  int16_t ax = bus.read() << 8 | bus.read();
  int16_t ay = bus.read() << 8 | bus.read();
  int16_t az = bus.read() << 8 | bus.read();
  bus.read(); bus.read();
  int16_t gx = bus.read() << 8 | bus.read();
  int16_t gy = bus.read() << 8 | bus.read();
  int16_t gz = bus.read() << 8 | bus.read();

  acc.x = (float)ax / 4096.0f * 9.81f;
  acc.y = (float)ay / 4096.0f * 9.81f;
  acc.z = (float)az / 4096.0f * 9.81f;

  const float RAD_PER_DEG = 3.14159265359f / 180.0f;
  gyro.x = ((float)gx / 65.5f) * RAD_PER_DEG;
  gyro.y = ((float)gy / 65.5f) * RAD_PER_DEG;
  gyro.z = ((float)gz / 65.5f) * RAD_PER_DEG;

  return true;
}

// ================= CALIBRATION =================
void calibrateSensors() {
  pinMode(LED_PIN, OUTPUT);
  Serial.println("Warmup... Place sensors STILL & UPRIGHT.");

  for(int i = 0; i < 10; i++) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(100);
  }
  digitalWrite(LED_PIN, HIGH);

  Serial.println("Calibrating gyro bias (2000 samples)... DO NOT MOVE.");

  const int samples = 2000;
  Vec3 sumBiasP(0,0,0), sumBiasL(0,0,0);
  Vec3 acc, gyro;
  int validP = 0, validL = 0;

  for (int i = 0; i < samples; i++) {
    if(readMPU(Wire, ADDR_PELVIS, acc, gyro)) { sumBiasP = sumBiasP + gyro; validP++; }
    if(readMPU(Wire1, ADDR_LUMBAR, acc, gyro)) { sumBiasL = sumBiasL + gyro; validL++; }
    delay(2);
  }

  biasPelvis = sumBiasP * (1.0f / (validP > 0 ? validP : 1));
  biasLumbar = sumBiasL * (1.0f / (validL > 0 ? validL : 1));

  digitalWrite(LED_PIN, LOW);
  isCalibrated = true;

  Serial.print("Pelvis Bias X: "); Serial.println(biasPelvis.x, 5);
  Serial.print("Lumbar Bias X: "); Serial.println(biasLumbar.x, 5);
}

// ================= BLE CALLBACKS =================
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer*) override { deviceConnected = true; Serial.println("BLE Connected"); }
  void onDisconnect(BLEServer* s) override {
    deviceConnected = false;
    authenticated = false;
    Serial.println("BLE Disconnected");
    s->startAdvertising();
  }
};

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String rxValue = pCharacteristic->getValue();
    if (rxValue == ACTIVATION_KEY) {
      authenticated = true;
      pTxCharacteristic->setValue("AUTH_SUCCESS");
      pTxCharacteristic->notify();

      memcpy(key, ACTIVATION_KEY, 16);
      memset(iv, 0, 16);
      mbedtls_aes_init(&aes);
      mbedtls_aes_setkey_enc(&aes, key, 128);

      Serial.println("Auth Success");
    } else {
      pTxCharacteristic->setValue("AUTH_FAIL");
      pTxCharacteristic->notify();
    }
  }
};

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  pinMode(HAPTIC_PIN, OUTPUT);
  digitalWrite(HAPTIC_PIN, LOW);

  delay(2000);
  Serial.println("\n===== ESP32 Smart Spine System =====");

  Wire.begin(SDA_PELVIS, SCL_PELVIS, 400000);
  Wire1.begin(SDA_LUMBAR, SCL_LUMBAR, 400000);
  delay(200);

  if (!initMPU(Wire,  ADDR_PELVIS, "Pelvis")) while(1);
  if (!initMPU(Wire1, ADDR_LUMBAR, "Lumbar")) while(1);

  filterPelvis.reset();
  filterLumbar.reset();
  detectorPelvis.setWindowSize(5);
  detectorLumbar.setWindowSize(5);

  // Gyro bias calibration (upright & still)
  calibrateSensors();

  // Set upright reference offset
  {
    Vec3 accP, gyroP, accL, gyroL;
    if (readMPU(Wire, ADDR_PELVIS, accP, gyroP)) filterPelvis.applyZUPT(accP);
    if (readMPU(Wire1, ADDR_LUMBAR, accL, gyroL)) filterLumbar.applyZUPT(accL);

    estimator.setUprightReference(filterPelvis.getQuaternion(), filterLumbar.getQuaternion());
    Serial.print("Upright reference set. Offset(deg)=");
    Serial.println(estimator.getZeroOffsetDeg(), 3);
  }

  lastMicros = micros();

  // BLE init
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pRx = pService->createCharacteristic(
    RX_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRx->setCallbacks(new MyCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
    TX_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  pService->start();
  BLEDevice::startAdvertising();

  Serial.println("System Ready.");
}

// ================= LOOP =================
void loop() {
  if (deviceConnected && authenticated) {
    unsigned long now = micros();
    float dt = (now - lastMicros) * 1e-6f;
    lastMicros = now;
    if (dt <= 0.0f || dt > 0.1f) dt = 0.01f;

    Vec3 accP, gyroP, accL, gyroL;

    // Pelvis
    if (readMPU(Wire, ADDR_PELVIS, accP, gyroP)) {
      gyroP = gyroP - biasPelvis;
      bool staticP = detectorPelvis.update(gyroP, accP, ZUPT_THRESHOLD);
      if (staticP) filterPelvis.applyZUPT(accP);
      else filterPelvis.update(gyroP, accP, dt);
    }

    // Lumbar
    if (readMPU(Wire1, ADDR_LUMBAR, accL, gyroL)) {
      gyroL = gyroL - biasLumbar;
      bool staticL = detectorLumbar.update(gyroL, accL, ZUPT_THRESHOLD);
      if (staticL) filterLumbar.applyZUPT(accL);
      else filterLumbar.update(gyroL, accL, dt);
    }

    Quaternion qP = filterPelvis.getQuaternion();
    Quaternion qL = filterLumbar.getQuaternion();

    estimator.update(qP, qL);
    estimator.handleHapticFeedback(HAPTIC_PIN);

    float rP, pP, yP, rL, pL, yL;
    quatToEuler(qP, rP, pP, yP);
    quatToEuler(qL, rL, pL, yL);

    const float R2D = 180.0f / M_PI;
    float relativeAngle = estimator.getAngle();

    String stateField = estimator.getCombinedStateString(); // "GREEN|FLEXION"

    String payload =
      String(pP * R2D, 1) + "," +
      String(rP * R2D, 1) + "," +
      String(yP * R2D, 1) + "," +
      String(pL * R2D, 1) + "," +
      String(rL * R2D, 1) + "," +
      String(yL * R2D, 1) + "," +
      String(relativeAngle, 1) + "," +
      stateField;

    size_t plainLen = payload.length();
    if (plainLen > MAX_PACKET_LEN) plainLen = MAX_PACKET_LEN;

    char plainBuf[MAX_PACKET_LEN + 1];
    payload.toCharArray(plainBuf, plainLen + 1);

    uint8_t encryptedBuf[MAX_PACKET_LEN];
    unsigned char nonce_counter[16];
    unsigned char stream_block[16];
    size_t nc_off = 0;

    memcpy(nonce_counter, iv, 16);
    mbedtls_aes_crypt_ctr(&aes, plainLen, &nc_off, nonce_counter, stream_block,
                          (unsigned char*)plainBuf, encryptedBuf);
    memcpy(iv, nonce_counter, 16);

    pTxCharacteristic->setValue(encryptedBuf, plainLen);
    pTxCharacteristic->notify();

    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 1000) {
      Serial.print("Angle: "); Serial.print(relativeAngle, 1);
      Serial.print(" deg | Zone: "); Serial.print(estimator.getZoneString());
      Serial.print(" | Case: "); Serial.println(estimator.getCaseString());
      lastPrint = millis();
    }
  }

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    oldDeviceConnected = deviceConnected;
    Serial.println("Restarting Advertising...");
  }
  if (deviceConnected && !oldDeviceConnected) oldDeviceConnected = deviceConnected;

  delay(10);
}
