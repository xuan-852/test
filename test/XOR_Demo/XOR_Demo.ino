#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

MAX30105 particleSensor;

// ==================== 配置参数 ====================
const byte RATE_SIZE = 4;
const int HRV_WINDOW = 20;//HRV窗口大小
const int FILTER_SIZE = 5;

// 信号阈值（已放宽）
const long IR_MIN = 30000;
const long IR_MAX = 250000;

// ==================== 全局变量 ====================
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute;
int beatAvg = 0;

long rrIntervals[HRV_WINDOW];
int rrIndex = 0;
bool rrFull = false;

long irBuffer[FILTER_SIZE];
int filterIndex = 0;

int currentLabel = -1;
unsigned long lastPrintTime = 0;

// 统计变量
int beatCount = 0;
unsigned long startTime = 0;

float calculateHRV();
long getFilteredIR();
void resetBuffers();

void setup() {
  Serial.begin(115200);
  while (!Serial); 

  Wire.begin(8, 9); 
  delay(1000); 

  Serial.println("================================================");
  Serial.println("        MAX30102 测试模式");
  Serial.println("================================================");
  Serial.println();
  Serial.println("正在初始化传感器...");

  bool sensorOK = false;
  if (particleSensor.begin(Wire, 100000)) sensorOK = true;
  else if (particleSensor.begin(Wire)) sensorOK = true;

  if (!sensorOK) {
    Serial.println("❌ 错误: 未找到 MAX30102 传感器");
    Serial.println("请检查接线:");
    Serial.println("  VCC -> 3.3V 或 5V");
    Serial.println("  GND -> GND");
    Serial.println("  SCL -> SCL (ESP32: GPIO 9)");
    Serial.println("  SDA -> SDA (ESP32: GPIO 8)");
    while (1);
  }

  Serial.println("✓ 传感器已连接");
  
  // 传感器配置
  particleSensor.setup(60, 4, 2, 400, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x3F);
  particleSensor.setPulseAmplitudeIR(0x3F);

  // 初始化缓冲区
  for (int i = 0; i < FILTER_SIZE; i++) irBuffer[i] = 0;
  for (int i = 0; i < HRV_WINDOW; i++) rrIntervals[i] = 0;
  for (int i = 0; i < RATE_SIZE; i++) rates[i] = 0;

  Serial.println("✓ 配置完成");
  Serial.println();
  Serial.println("================================================");
  Serial.println("测试说明:");
  Serial.println("  1. 将手指轻放在传感器上");
  Serial.println("  2. 观察 IR 值是否在合理范围");
  Serial.println("  3. 等待心跳检测 (会显示 ♥)");
  Serial.println("  4. 发送 'c' 或 'e' 开始记录数据");
  Serial.println("  5. 发送 's' 停止记录");
  Serial.println("================================================");
  Serial.println();
  Serial.println("输出格式:");
  Serial.println("  [IR值] [状态] BPM=xx HRV=xx");
  Serial.println();
  Serial.println("------------------------------------------------");
  
  startTime = millis();
}

void loop() {
  // 检查串口指令
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'c') {
      currentLabel = 0;
      Serial.println("\n>>> 开始记录: 平静状态 (label=0)");
      Serial.println("BPM,HRV,LABEL");
    } else if (command == 'e') {
      currentLabel = 1;
      Serial.println("\n>>> 开始记录: 兴奋状态 (label=1)");
      Serial.println("BPM,HRV,LABEL");
    } else if (command == 's') {
      currentLabel = -1;
      Serial.println("\n>>> 停止记录");
    }
  }

  // 读取信号
  long rawIR = particleSensor.getIR();
  long irValue = getFilteredIR();

  // 显示原始 IR 值（每 200ms 更新一次）
  static unsigned long lastDisplayTime = 0;
  if (millis() - lastDisplayTime > 200) {
    // 判断信号状态
    String status = "";
    if (rawIR < IR_MIN) {
      status = "❌ 信号太弱";
    } else if (rawIR > IR_MAX) {
      status = "⚠ 信号饱和";
    } else {
      status = "✓ 正常";
    }
    
    // 显示状态
    Serial.print("IR: ");
    Serial.print(rawIR);
    Serial.print(" (滤波: ");
    Serial.print(irValue);
    Serial.print(") ");
    Serial.print(status);
    
    // 显示当前 BPM 和 HRV
    if (beatAvg > 0) {
      Serial.print(" | BPM=");
      Serial.print(beatAvg);
    }
    
    float hrv = calculateHRV();
    if (hrv > 0) {
      Serial.print(" HRV=");
      Serial.print(hrv, 1);
    }
    
    // 显示心跳计数
    if (beatCount > 0) {
      Serial.print(" | 心跳: ");
      Serial.print(beatCount);
    }
    
    Serial.println();
    
    lastDisplayTime = millis();
  }

  // 信号质量检测
  if (irValue < IR_MIN) {
    resetBuffers();
    delay(50);
    return;
  }

  if (irValue > IR_MAX) {
    delay(50);
    return;
  }

  // 心跳检测
  if (checkForBeat(irValue) == true) {
    long now = millis();
    long delta = now - lastBeat;
    lastBeat = now;

    // 显示心跳标记
    Serial.println("                                    ♥ 心跳检测!");

    if (delta > 300 && delta < 1500) {
      // 存储 RR 间隔
      rrIntervals[rrIndex] = delta;
      rrIndex = (rrIndex + 1) % HRV_WINDOW;
      if (rrIndex == 0) rrFull = true;

      // 计算 BPM
      beatsPerMinute = 60.0 / (delta / 1000.0);

      Serial.print("                                    RR间隔: ");
      Serial.print(delta);
      Serial.print("ms -> BPM: ");
      Serial.println(beatsPerMinute, 1);

      if (beatsPerMinute > 40 && beatsPerMinute < 200) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;

        beatAvg = 0;
        int validCount = 0;
        for (byte x = 0; x < RATE_SIZE; x++) {
          if (rates[x] > 0) {
            beatAvg += rates[x];
            validCount++;
          }
        }
        if (validCount > 0) beatAvg /= validCount;
        
        beatCount++;
      }
    }
  }

  // 输出 CSV 数据（如果正在记录）
  if (millis() - lastPrintTime > 500 && currentLabel != -1) {
    float hrv = calculateHRV();
    
    if (beatAvg > 0 && hrv > 0) {
      Serial.print(beatAvg);
      Serial.print(",");
      Serial.print(hrv);
      Serial.print(",");
      Serial.println(currentLabel);
    }
    lastPrintTime = millis();
  }
}

long getFilteredIR() {
  irBuffer[filterIndex] = particleSensor.getIR();
  filterIndex = (filterIndex + 1) % FILTER_SIZE;
  
  long sum = 0;
  for (int i = 0; i < FILTER_SIZE; i++) {
    sum += irBuffer[i];
  }
  return sum / FILTER_SIZE;
}

float calculateHRV() {
  int count = rrFull ? HRV_WINDOW : rrIndex;
  if (count < 5) return 0.0;
  
  long sum = 0;
  for (int i = 0; i < count; i++) {
    sum += rrIntervals[i];
  }
  float avgRR = sum / (float)count;
  
  long sumSquaredDifferences = 0;
  int validCount = 0;
  
  for (int i = 1; i < count; i++) {
    if (abs(rrIntervals[i] - avgRR) < avgRR * 0.3) {
      long diff = rrIntervals[i] - rrIntervals[i-1];
      sumSquaredDifferences += (diff * diff);
      validCount++;
    }
  }
  
  if (validCount < 3) return 0.0;
  return sqrt(sumSquaredDifferences / (float)validCount);
}

void resetBuffers() {
  beatAvg = 0;
  rrIndex = 0;
  rrFull = false;
  beatCount = 0;
  for (int i = 0; i < RATE_SIZE; i++) rates[i] = 0;
}
