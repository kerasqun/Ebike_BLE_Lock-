/*
 * 两轮电动车 BLE 手机解锁模块  v0.2.1
 * 硬件: ESP32-C3 + 5V 继电器(光耦,COM/NO 并联电门锁) + 有源蜂鸣器 + SW-420
 * 依赖库: NimBLE-Arduino 2.x
 *
 * 产品逻辑 (v0.2.1):
 *   - 靠近只做"准备"(armed)，不立刻通电；在准备态下震动(捏刹车/轻推车)才吸合继电器
 *   - 人离开 30 秒后自动断电
 *   - 锁车态下震动触发防盗报警(与开锁震动窗口区分)
 *   - iPhone 无感: 请求 ANCS 让系统维持连接(锁屏/杀 App 不断)，读连接 RSSI
 *
 * 手机指令 (写入 CMD 特征值, UTF-8):
 *   UNLOCK:<密码>            强制开锁
 *   LOCK:<密码>              关锁
 *   FIND:<密码>              寻车鸣叫
 *   BIND:<密码>              绑定当前已配对手机为可信设备
 *   BEACON:<密码>:<MAC>      绑定固定地址手环/信标
 *   AUTO:<密码>:1|0          开/关靠近自动准备
 *   PWD:<旧密码>:<新密码>    改密码
 * STATE notify: LOCKED / ARMED / UNLOCKED / ALARM / OK / ERR:...
 */
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_pm.h"

// ================== 引脚定义 (集中在这里改) ==================
#define PIN_RELAY        3
#define RELAY_ACTIVE     HIGH   // 低电平触发模块改成 LOW
#define PIN_BUZZER       4
#define PIN_VIBRATION    5
#define PIN_LED          8      // SuperMini 板载 LED; 不用设 -1

// ================== 参数 ==================
#define DEFAULT_PASSWORD   "123456"
#define RSSI_NEAR          (-70)   // 进入准备区
#define RSSI_FAR           (-80)   // 离开(滞回)
#define LEAVE_TIMEOUT_MS   30000
#define ARM_SETTLE_MS      400     // 进入准备后短延时, 避免刚靠近就误触
#define UNLOCK_VIB_N       1       // 准备态下震动次数达到即开锁
#define UNLOCK_VIB_WIN_MS  1500
#define ALARM_VIB_N        3
#define ALARM_VIB_WIN_MS   2000
#define ALARM_MS           8000
#define ALARM_COOLDOWN_MS  15000
#define DEVICE_NAME        "EBike-Lock"

#define SVC_UUID   "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CMD_UUID   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define STATE_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Apple ANCS
static NimBLEUUID ANCS_SVC("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static NimBLEUUID ANCS_NS ("9FBF120D-6301-42D9-8C58-25E699A21DBD"); // Notification Source
static NimBLEUUID ANCS_CP ("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"); // Control Point
static NimBLEUUID ANCS_DS ("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"); // Data Source

Preferences prefs;
NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *stateChr = nullptr;
NimBLEClient *ancsClient = nullptr;
String password;
bool unlocked = false;
bool armed = false;          // 靠近准备态
bool autoMode = true;
String boundId;
String beaconMac;
uint16_t connHandle = 0xFFFF;
bool trustedConnected = false;
bool ancsSubscribed = false;
volatile uint32_t vibPulses = 0;
uint32_t vibWindowStart = 0, alarmUntil = 0, alarmCooldown = 0;
uint32_t lastNear = 0, armedSince = 0;
int lastBeaconRssi = -127;
uint32_t lastBeaconSeen = 0;

void notify(const String &s) {
  if (!stateChr) return;
  stateChr->setValue(s.c_str());
  stateChr->notify();
}

void setRelay(bool on, bool save = true) {
  unlocked = on;
  if (on) armed = false;
  digitalWrite(PIN_RELAY, on ? RELAY_ACTIVE : !RELAY_ACTIVE);
  if (PIN_LED >= 0) digitalWrite(PIN_LED, on ? LOW : HIGH);
  if (save) prefs.putBool("unlocked", on);
  if (stateChr) notify(on ? "UNLOCKED" : (armed ? "ARMED" : "LOCKED"));
  Serial.printf("[relay] %s\n", on ? "UNLOCK" : "LOCK");
}

void setArmed(bool on) {
  if (unlocked) return;
  if (armed == on) return;
  armed = on;
  if (on) {
    armedSince = millis();
    beep(1, 40);
    notify("ARMED");
    Serial.println("[prox] ARMED");
  } else {
    notify("LOCKED");
    Serial.println("[prox] DISARM");
  }
}

void beep(int times, int onMs = 120, int offMs = 120) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(onMs);
    digitalWrite(PIN_BUZZER, LOW);  delay(offMs);
  }
}

// ================== ANCS: 让 iOS 系统维持连接 ==================
class AncsClientCb : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *c) override {
    Serial.println("[ancs] client connected (same peer)");
  }
  void onDisconnect(NimBLEClient *c, int reason) override {
    ancsSubscribed = false;
    Serial.printf("[ancs] client disconnect reason=%d\n", reason);
  }
};

void trySubscribeAncs(NimBLEConnInfo &info) {
  // 对已建立的服务器连接发起对端服务发现并订阅 ANCS Notification Source。
  // iOS 在订阅 ANCS 后会由系统保活这条链路(锁屏/杀 App 不断)。
  if (ancsSubscribed) return;
  if (!info.isEncrypted()) return;

  if (!ancsClient) {
    ancsClient = NimBLEDevice::createClient();
    ancsClient->setClientCallbacks(new AncsClientCb(), false);
  }
  if (ancsClient->isConnected()) return;

  // 复用已有连接: NimBLE 2.x 用 connect(peerAddress) 走已有链路时需 peer 支持。
  // 这里用对端地址再连一次(同一 ACL 上作为 GATT Client)。
  NimBLEAddress peer = info.getAddress();
  if (!ancsClient->connect(peer, false)) {
    Serial.println("[ancs] connect failed");
    return;
  }
  NimBLERemoteService *svc = ancsClient->getService(ANCS_SVC);
  if (!svc) {
    Serial.println("[ancs] ANCS service not found (非 iPhone 或未授权通知访问)");
    ancsClient->disconnect();
    return;
  }
  NimBLERemoteCharacteristic *ns = svc->getCharacteristic(ANCS_NS);
  if (!ns || !ns->canNotify()) {
    Serial.println("[ancs] NS char missing");
    ancsClient->disconnect();
    return;
  }
  // 订阅即可触发 iOS 弹"允许通知"并保活; 通知内容我们不解析
  bool ok = ns->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t*, size_t, bool){});
  ancsSubscribed = ok;
  Serial.printf("[ancs] subscribe NS -> %s\n", ok ? "OK" : "FAIL");
  // 可选: 也订 Data Source, 部分机型更稳
  NimBLERemoteCharacteristic *ds = svc->getCharacteristic(ANCS_DS);
  if (ds && ds->canNotify()) ds->subscribe(true, [](NimBLERemoteCharacteristic*, uint8_t*, size_t, bool){});
}

// ================== 指令 ==================
void handleCmd(const std::string &raw, NimBLEConnInfo &info) {
  String s = String(raw.c_str()); s.trim();
  int p1 = s.indexOf(':');
  if (p1 < 0) { notify("ERR"); return; }
  String cmd = s.substring(0, p1);
  String rest = s.substring(p1 + 1);
  int p2 = rest.indexOf(':');
  String pwd = p2 < 0 ? rest : rest.substring(0, p2);
  String arg = p2 < 0 ? "" : rest.substring(p2 + 1);

  if (pwd != password) { notify("ERR:PWD"); beep(1, 400); return; }

  if (cmd == "UNLOCK") {
    setRelay(true); beep(1); lastNear = millis();
  } else if (cmd == "LOCK") {
    setRelay(false); setArmed(false); beep(2);
  } else if (cmd == "FIND") {
    beep(6, 200, 150); notify("OK");
  } else if (cmd == "BIND") {
    if (!info.isBonded()) { notify("ERR:NOT_BONDED"); return; }
    boundId = String(info.getIdAddress().toString().c_str());
    prefs.putString("bound", boundId);
    trustedConnected = true;
    notify("OK");
    trySubscribeAncs(info);
  } else if (cmd == "BEACON") {
    beaconMac = arg; beaconMac.toLowerCase();
    prefs.putString("beacon", beaconMac); notify("OK");
  } else if (cmd == "AUTO") {
    autoMode = (arg == "1"); prefs.putBool("auto", autoMode); notify("OK");
  } else if (cmd == "PWD" && arg.length() >= 4) {
    password = arg; prefs.putString("pwd", password); notify("OK");
  } else {
    notify("ERR");
  }
}

class CmdCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c, NimBLEConnInfo &info) override {
    handleCmd(c->getValue(), info);
  }
};

class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &info) override {
    connHandle = info.getConnHandle();
    NimBLEDevice::startSecurity(connHandle);
  }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    String id = String(info.getIdAddress().toString().c_str());
    trustedConnected = (boundId.length() && id == boundId);
    Serial.printf("[ble] auth id=%s trusted=%d\n", id.c_str(), trustedConnected);
    if (trustedConnected) trySubscribeAncs(info);
  }
  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &info, int reason) override {
    connHandle = 0xFFFF;
    trustedConnected = false;
    ancsSubscribed = false;
    if (ancsClient && ancsClient->isConnected()) ancsClient->disconnect();
    NimBLEDevice::startAdvertising();
  }
};

class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    if (beaconMac.length() && String(d->getAddress().toString().c_str()) == beaconMac) {
      lastBeaconRssi = d->getRSSI();
      lastBeaconSeen = millis();
    }
  }
};

int currentRssi() {
  int best = -127;
  if (trustedConnected && connHandle != 0xFFFF) {
    int8_t r;
    if (ble_gap_conn_rssi(connHandle, &r) == 0) best = r;
  }
  if (millis() - lastBeaconSeen < 5000 && lastBeaconRssi > best) best = lastBeaconRssi;
  return best;
}

void proximityTask() {
  if (!autoMode) return;
  static uint32_t t = 0;
  if (millis() - t < 500) return;
  t = millis();

  int rssi = currentRssi();
  if (rssi > RSSI_NEAR) {
    lastNear = millis();
    if (!unlocked && !armed) setArmed(true);
  } else if (rssi > RSSI_FAR) {
    lastNear = millis(); // 滞回区保持
  } else {
    // 离开范围
    if (armed) setArmed(false);
    if (unlocked && millis() - lastNear > LEAVE_TIMEOUT_MS) {
      setRelay(false);
      beep(2);
    }
  }
}

void IRAM_ATTR onVib() { vibPulses++; }

void vibrationTask() {
  uint32_t now = millis();

  // 报警进行中
  if (alarmUntil) {
    digitalWrite(PIN_BUZZER, (now / 250) % 2);
    if (now > alarmUntil || unlocked) {
      alarmUntil = 0;
      digitalWrite(PIN_BUZZER, LOW);
      alarmCooldown = now + ALARM_COOLDOWN_MS;
    }
    return;
  }

  // 准备态 + 震动 -> 开锁 (无感: 捏刹车/轻推车)
  if (armed && !unlocked && now - armedSince >= ARM_SETTLE_MS) {
    static uint32_t unlockWinStart = 0;
    static uint32_t unlockCount = 0;
    if (now - unlockWinStart > UNLOCK_VIB_WIN_MS) {
      unlockWinStart = now;
      unlockCount = 0;
    }
    // 用全局脉冲增量
    static uint32_t lastSeen = 0;
    uint32_t cur = vibPulses;
    if (cur != lastSeen) {
      unlockCount += (cur - lastSeen);
      lastSeen = cur;
    }
    if (unlockCount >= UNLOCK_VIB_N) {
      setRelay(true);
      beep(1);
      lastNear = now;
      unlockCount = 0;
      return;
    }
  }

  // 锁车且未准备: 防盗报警
  if (!unlocked && !armed && now >= alarmCooldown) {
    if (now - vibWindowStart > ALARM_VIB_WIN_MS) {
      vibWindowStart = now;
      // 窗口重置时不把全局计数清零, 用差值
    }
    static uint32_t alarmBase = 0;
    static uint32_t alarmWinAt = 0;
    if (now - alarmWinAt > ALARM_VIB_WIN_MS) {
      alarmWinAt = now;
      alarmBase = vibPulses;
    }
    if (vibPulses - alarmBase >= ALARM_VIB_N) {
      alarmUntil = now + ALARM_MS;
      alarmBase = vibPulses;
      notify("ALARM");
    }
  } else if (unlocked || armed) {
    // 开锁/准备期间不累计防盗
  }
}

void setup() {
  Serial.begin(115200);
  // 先写好电平再切 OUTPUT, 避免上电瞬间继电器误吸合(高电平触发时默认低=断开)
  digitalWrite(PIN_RELAY, !RELAY_ACTIVE);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BUZZER, OUTPUT);
  if (PIN_LED >= 0) { digitalWrite(PIN_LED, HIGH); pinMode(PIN_LED, OUTPUT); }
  pinMode(PIN_VIBRATION, INPUT_PULLUP);

  prefs.begin("ebike", false);
  password  = prefs.getString("pwd", DEFAULT_PASSWORD);
  autoMode  = prefs.getBool("auto", true);
  boundId   = prefs.getString("bound", "");
  beaconMac = prefs.getString("beacon", "");

  setCpuFrequencyMhz(80);
  esp_pm_config_t pm = { .max_freq_mhz = 80, .min_freq_mhz = 10, .light_sleep_enable = true };
  esp_pm_configure(&pm);

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(3);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new SrvCb());
  NimBLEService *svc = bleServer->createService(SVC_UUID);
  NimBLECharacteristic *cmd = svc->createCharacteristic(
      CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  cmd->setCallbacks(new CmdCb());
  stateChr = svc->createCharacteristic(
      STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);
  adv->addServiceUUID(SVC_UUID);
  adv->setMinInterval(800);
  adv->setMaxInterval(1600);
  adv->start();

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCb(), true);
  scan->setActiveScan(false);
  scan->setWindow(48);
  scan->start(0, false, true);

  setRelay(prefs.getBool("unlocked", false), false);
  attachInterrupt(digitalPinToInterrupt(PIN_VIBRATION), onVib, CHANGE);
  beep(1, 60);
  Serial.println("[boot] v0.2.1 ready (ANCS + arm-then-vib)");
}

void loop() {
  proximityTask();
  vibrationTask();
  if (!beaconMac.length() && NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->stop();
  }
  delay(20);
}
