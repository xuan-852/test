// MAX30102 心率和 HRV 测试程序
// 适用于 ESP32，使用 MAX30105 库
#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

MAX30105 particleSensor;// 创建传感器对象

// ==================== 配置参数 ====================
const byte RATE_SIZE = 4;
const int HRV_WINDOW = 20;
const int FILTER_SIZE = 5;

const long IR_MIN = 30000;//信号下限
const long IR_MAX = 250000;//信号上限

byte rates[RATE_SIZE];//心跳数据存储位
byte rateSpot = 0;//心跳数据存储位索引

long lastBeat = 0;// 上次心跳时间
float beatsPerMinute;// 心率
int beatAvg = 0;//心跳平均值

// HRV 相关参数
long rrIntervals[HRV_WINDOW];//RR间隔数据存储位
int rrIndex = 0;//rrInterval 的索引值
bool rrFull = false;

long irBuffer[FILTER_SIZE];//IR数据滤波缓冲区
int filterIndex = 0;//IR数据滤波缓冲区索引

int currentLabel = -1;//标志位 -1=不记录 0=平静 1=兴奋
unsigned long lastPrintTime = 0;//上次打印时间  

int beatCount = 0;//心跳计数
unsigned long startTime = 0;//开始时间

// 函数声明
float calculateHRV();
long getFilteredIR();
void resetBuffers();
void error_hendler(void);

void setup() {
  Serial.begin(115200);
  while (!Serial) {
  }

  Wire.begin(8, 9);//设置I2C的引脚为GPIO 8和GPIO 9
  delay(1000);

  Serial.println("================================================");
  Serial.println("        MAX30102 测试模式");
  Serial.println("================================================");
  Serial.println();
  Serial.println("正在初始化传感器...");

  bool sensorOK = false;//I2C连接状态标识符
  if (particleSensor.begin(Wire, 100000)) sensorOK = true;
  else if (particleSensor.begin(Wire)) sensorOK = true;
  // 检查传感器是否连接成功，并反馈给用户，用作测试，确定可用可删除
  if (!sensorOK) {
    Serial.println("错误: 未找到 MAX30102 传感器");
    Serial.println("请检查接线:");
    Serial.println("  VCC -> 3.3V 或 5V");
    Serial.println("  GND -> GND");
    Serial.println("  SCL -> SCL (ESP32: GPIO 9)");
    Serial.println("  SDA -> SDA (ESP32: GPIO 8)");
    error_hendler();
  }

  Serial.println("传感器已连接");

  particleSensor.setup(60, 4, 2, 400, 411, 4096);// 配置传感器参数,括号内数据表示如下 : 
  //接收的LED功率,平均采样数,LED模式（0=单色,1=双色）,采样率,脉冲宽度,ADC范围
  particleSensor.setPulseAmplitudeRed(0x3F);// 设置红光LED的功率
  particleSensor.setPulseAmplitudeIR(0x3F);// 设置红外LED的功率

  // 初始化滤波和HRV缓冲区
  for (int i = 0; i < FILTER_SIZE; i++) irBuffer[i] = 0;
  for (int i = 0; i < HRV_WINDOW; i++) rrIntervals[i] = 0;
  for (int i = 0; i < RATE_SIZE; i++) rates[i] = 0;

  Serial.println("配置完成");
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

  startTime = millis();// 记录开始时间
}

void loop() {
  
  //这里是读取用户自身数据用的
  if (Serial.available()) {// 检查串口是否有数据
    char command = Serial.read();// 读取串口命令
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

  long rawIR = particleSensor.getIR();// 读取原始 IR 值
  long irValue = getFilteredIR();// 获取滤波后的 IR 值

  static unsigned long lastDisplayTime = 0;
  if (millis() - lastDisplayTime > 200) {
    String status = "";
    if (rawIR < IR_MIN) {
      status = "信号太弱";
    } else if (rawIR > IR_MAX) {
      status = "信号饱和";
    } else {
      status = "正常";
    }

    Serial.print("IR: ");
    Serial.print(rawIR);
    Serial.print(" (滤波后的IR值: ");
    Serial.print(irValue);
    Serial.print(") ");
    Serial.print(status);

    if (beatAvg > 0) {
      Serial.print(" | BPM=");
      Serial.print(beatAvg);
    }

    float hrv = calculateHRV();
    if (hrv > 0) {
      Serial.print(" HRV=");
      Serial.print(hrv, 1);
    }

    if (beatCount > 0) {
      Serial.print(" | 心跳计数次数: ");
      Serial.print(beatCount);
    }

    Serial.println();
    lastDisplayTime = millis();
  }
// 信号质量检测和心跳检测
  if (irValue < IR_MIN) {
    resetBuffers();
    delay(50);
    return;
  }

  if (irValue > IR_MAX) {
    delay(50);
    return;
  }

  if (checkForBeat(irValue) == true) {
    long now = millis();
    long delta = now - lastBeat;
    lastBeat = now;

    Serial.println("                                    心跳检测!");

    if (delta > 300 && delta < 1500) {
      rrIntervals[rrIndex] = delta;
      rrIndex = (rrIndex + 1) % HRV_WINDOW;
      if (rrIndex == 0) rrFull = true;

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

long getFilteredIR() {//用平均值来减小误差
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
      long diff = rrIntervals[i] - rrIntervals[i - 1];
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

void error_hendler(void) {
  Serial.println("发生错误，请检查传感器连接和配置!");
  while (1) {
    // 错误处理循环
  }
}