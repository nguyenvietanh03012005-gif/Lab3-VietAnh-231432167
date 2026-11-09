#include <WiFi.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <Adafruit_BMP085.h>
#include <math.h>
#include <stdarg.h>

#define WIFI_SSID      "Wokwi-GUEST"
#define WIFI_PASS      ""
#define WIFI_CHANNEL   6

#define MQTT_HOST      "broker.hivemq.com"
#define MQTT_PORT      1883
#define TOPIC_BASE     "esp32/runner/vietanh"

#define TOPIC_DATA       TOPIC_BASE "/data"
#define TOPIC_STATUS     TOPIC_BASE "/status"
#define TOPIC_ONLINE     TOPIC_BASE "/online"
#define TOPIC_CMD_ALL    TOPIC_BASE "/cmd/#"
#define TOPIC_CMD_MODE   TOPIC_BASE "/cmd/mode"
#define TOPIC_CMD_LED    TOPIC_BASE "/cmd/led"
#define TOPIC_CMD_BUZZER TOPIC_BASE "/cmd/buzzer"

#define PIN_SDA        21
#define PIN_SCL        22
#define PIN_BUZZER     25
#define PIN_LED_ALERT  26
#define PIN_LED_NET    27

#define TEMP_HIGH       38.0f
#define TEMP_LOW        35.0f
#define ALT_RATE_MAX    2.0f
#define ALT_ALPHA       0.5f
#define ALT_ALARM_MS    5000
#define SEA_LEVEL_PA    101325.0f
#define SAMPLE_MS       1000

#define BUZ_FREQ_TEMP   2000
#define BUZ_FREQ_ALT    3000
#define BUZ_FREQ_MANUAL 2500

typedef struct {
  float    temperature;
  float    pressure;
  float    altitude;
  uint32_t ts;
} SensorData_t;

typedef struct {
  float    temperature;
  float    altitude;
  uint32_t ts;
} AlertData_t;

typedef enum {
  CMD_MODE_AUTO, CMD_MODE_MANUAL,
  CMD_LED_ON, CMD_LED_OFF,
  CMD_BUZ_ON, CMD_BUZ_OFF
} CmdType_t;

typedef struct { CmdType_t type; } Command_t;

#define ALARM_NONE       0x00
#define ALARM_TEMP_HIGH  0x01
#define ALARM_TEMP_LOW   0x02
#define ALARM_ALT        0x04

typedef struct {
  bool    manual;
  bool    led;
  bool    buzzer;
  uint8_t alarm;
} Status_t;

QueueHandle_t     dataQueue;
QueueHandle_t     alertQueue;
QueueHandle_t     cmdQueue;
QueueHandle_t     statusQueue;
SemaphoreHandle_t serialMutex;
SemaphoreHandle_t netReadySem;

Adafruit_BMP085 bmp;
WiFiClient      espClient;
PubSubClient    mqtt(espClient);

void logf(const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.println(buf);
    xSemaphoreGive(serialMutex);
  }
}

void queueSendLatest(QueueHandle_t q, const void *item) {
  if (xQueueSend(q, item, 0) != pdTRUE) {
    uint8_t dump[32];
    xQueueReceive(q, dump, 0);
    xQueueSend(q, item, 0);
  }
}

float pressureToAltitude(float pa) {
  return 44330.0f * (1.0f - powf(pa / SEA_LEVEL_PA, 0.1903f));
}

const char *alarmToText(uint8_t a, char *buf, size_t n) {
  if (a == ALARM_NONE) { snprintf(buf, n, "NONE"); return buf; }
  snprintf(buf, n, "%s%s%s",
           (a & ALARM_TEMP_HIGH) ? "TEMP_HIGH " : "",
           (a & ALARM_TEMP_LOW)  ? "TEMP_LOW "  : "",
           (a & ALARM_ALT)       ? "ALT "       : "");
  size_t len = strlen(buf);
  if (len && buf[len - 1] == ' ') buf[len - 1] = '\0';
  return buf;
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  char msg[16];
  unsigned int n = len < sizeof(msg) - 1 ? len : sizeof(msg) - 1;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  Command_t cmd;
  bool ok = true;
  if (strcmp(topic, TOPIC_CMD_MODE) == 0) {
    if      (strcasecmp(msg, "AUTO") == 0)   cmd.type = CMD_MODE_AUTO;
    else if (strcasecmp(msg, "MANUAL") == 0) cmd.type = CMD_MODE_MANUAL;
    else ok = false;
  } else if (strcmp(topic, TOPIC_CMD_LED) == 0) {
    if      (strcasecmp(msg, "ON") == 0)  cmd.type = CMD_LED_ON;
    else if (strcasecmp(msg, "OFF") == 0) cmd.type = CMD_LED_OFF;
    else ok = false;
  } else if (strcmp(topic, TOPIC_CMD_BUZZER) == 0) {
    if      (strcasecmp(msg, "ON") == 0)  cmd.type = CMD_BUZ_ON;
    else if (strcasecmp(msg, "OFF") == 0) cmd.type = CMD_BUZ_OFF;
    else ok = false;
  } else {
    ok = false;
  }

  if (ok) {
    xQueueSend(cmdQueue, &cmd, 0);
    logf("[MQTT] <- %s = %s", topic, msg);
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  logf("[WiFi] Dang ket noi %s ...", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  logf("[WiFi] Da ket noi, IP: %s", WiFi.localIP().toString().c_str());
}

bool connectMQTT() {
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "esp32-runner-%08lX", (unsigned long)esp_random());
  logf("[MQTT] Ket noi %s:%d (id=%s) ...", MQTT_HOST, MQTT_PORT, clientId);

  if (mqtt.connect(clientId, TOPIC_ONLINE, 1, true, "0")) {
    mqtt.publish(TOPIC_ONLINE, "1", true);
    mqtt.subscribe(TOPIC_CMD_ALL);
    logf("[MQTT] Da ket noi, subscribe %s", TOPIC_CMD_ALL);
    return true;
  }
  logf("[MQTT] That bai, rc=%d. Thu lai sau 2 s", mqtt.state());
  return false;
}

void taskSendData(void *pv) {
  SensorData_t d;
  Status_t st;
  char payload[160], alarmTxt[32];

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(PIN_LED_NET, LOW);
      connectWiFi();
    }

    if (!mqtt.connected()) {
      digitalWrite(PIN_LED_NET, LOW);
      if (connectMQTT()) {
        digitalWrite(PIN_LED_NET, HIGH);
        xSemaphoreGive(netReadySem);
      } else {
        vTaskDelay(pdMS_TO_TICKS(2000));
        continue;
      }
    }
    mqtt.loop();

    while (xQueueReceive(statusQueue, &st, 0) == pdTRUE) {
      snprintf(payload, sizeof(payload),
               "{\"mode\":\"%s\",\"led\":%s,\"buzzer\":%s,\"alarm\":\"%s\"}",
               st.manual ? "MANUAL" : "AUTO",
               st.led ? "true" : "false",
               st.buzzer ? "true" : "false",
               alarmToText(st.alarm, alarmTxt, sizeof(alarmTxt)));
      mqtt.publish(TOPIC_STATUS, payload, true);
      logf("[MQTT] -> status %s", payload);
    }

    if (xQueueReceive(dataQueue, &d, pdMS_TO_TICKS(100)) == pdTRUE) {
      snprintf(payload, sizeof(payload),
               "{\"temperature\":%.2f,\"pressure\":%.2f,\"altitude\":%.2f,\"ts\":%lu}",
               d.temperature, d.pressure / 100.0f, d.altitude, (unsigned long)d.ts);
      bool ok = mqtt.publish(TOPIC_DATA, payload);
      logf("[MQTT] -> data %s %s", payload, ok ? "OK" : "LOI");
    }
  }
}

void taskReadData(void *pv) {
  SensorData_t d;
  AlertData_t a;
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    d.temperature = bmp.readTemperature();
    d.pressure    = (float)bmp.readPressure();
    d.altitude    = pressureToAltitude(d.pressure);
    d.ts          = millis();

    logf("[BMP180] T = %.2f C | P = %.2f hPa | H = %.2f m",
         d.temperature, d.pressure / 100.0f, d.altitude);

    queueSendLatest(dataQueue, &d);

    a.temperature = d.temperature;
    a.altitude    = d.altitude;
    a.ts          = d.ts;
    queueSendLatest(alertQueue, &a);

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

void taskAlert(void *pv) {
  AlertData_t a;
  Command_t cmd;
  char alarmTxt[32];

  bool manual = false, manLed = false, manBuz = false;
  uint8_t tempAlarm = ALARM_NONE, alarm = ALARM_NONE;
  float altFilt = NAN, altPrev = NAN;
  uint32_t tPrev = 0, altAlarmUntil = 0;
  bool hasAltAlarm = false;

  bool ledOut = false, buzOut = false;
  uint16_t buzFreqOut = 0;
  Status_t last = {false, false, false, 0xFF};
  bool forcePublish = true;

  for (;;) {
    if (xQueueReceive(alertQueue, &a, pdMS_TO_TICKS(100)) == pdTRUE) {
      if      (a.temperature > TEMP_HIGH) tempAlarm = ALARM_TEMP_HIGH;
      else if (a.temperature < TEMP_LOW)  tempAlarm = ALARM_TEMP_LOW;
      else                                tempAlarm = ALARM_NONE;

      if (isnan(altFilt)) {
        altFilt = altPrev = a.altitude;
        tPrev = a.ts;
      } else {
        altFilt = ALT_ALPHA * a.altitude + (1.0f - ALT_ALPHA) * altFilt;
        float dt = (a.ts - tPrev) / 1000.0f;
        if (dt > 0.0f) {
          float rate = (altFilt - altPrev) / dt;
          if (fabsf(rate) > ALT_RATE_MAX) {
            altAlarmUntil = millis() + ALT_ALARM_MS;
            hasAltAlarm = true;
            logf("[ALERT] Do cao thay doi dot ngot: %.2f m/s", rate);
          }
        }
        altPrev = altFilt;
        tPrev = a.ts;
      }
    }

    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case CMD_MODE_AUTO:   manual = false;                break;
        case CMD_MODE_MANUAL: manual = true;                 break;
        case CMD_LED_ON:      manual = true; manLed = true;  break;
        case CMD_LED_OFF:     manual = true; manLed = false; break;
        case CMD_BUZ_ON:      manual = true; manBuz = true;  break;
        case CMD_BUZ_OFF:     manual = true; manBuz = false; break;
      }
    }

    if (hasAltAlarm && (int32_t)(altAlarmUntil - millis()) <= 0) hasAltAlarm = false;
    uint8_t newAlarm = tempAlarm | (hasAltAlarm ? ALARM_ALT : ALARM_NONE);
    if (newAlarm != alarm) {
      alarm = newAlarm;
      logf("[ALERT] Trang thai canh bao: %s", alarmToText(alarm, alarmTxt, sizeof(alarmTxt)));
    }

    uint32_t ph = millis() / 100;
    bool wantLed, wantBuz;
    uint16_t freq;
    if (manual) {
      wantLed = manLed; wantBuz = manBuz; freq = BUZ_FREQ_MANUAL;
    } else if (alarm & ALARM_ALT) {
      wantLed = (ph % 2) == 0; wantBuz = true; freq = BUZ_FREQ_ALT;
    } else if (alarm & ALARM_TEMP_HIGH) {
      wantLed = true; wantBuz = (ph % 4) < 2; freq = BUZ_FREQ_TEMP;
    } else if (alarm & ALARM_TEMP_LOW) {
      wantLed = (ph % 10) < 5; wantBuz = (ph % 10) < 2; freq = BUZ_FREQ_TEMP;
    } else {
      wantLed = false; wantBuz = false; freq = 0;
    }

    if (wantLed != ledOut) {
      digitalWrite(PIN_LED_ALERT, wantLed ? HIGH : LOW);
      ledOut = wantLed;
    }
    if (wantBuz != buzOut || (wantBuz && freq != buzFreqOut)) {
      if (wantBuz) tone(PIN_BUZZER, freq); else noTone(PIN_BUZZER);
      buzOut = wantBuz;
      buzFreqOut = wantBuz ? freq : 0;
    }

    if (xSemaphoreTake(netReadySem, 0) == pdTRUE) forcePublish = true;
    Status_t st;
    st.manual = manual;
    st.led    = manual ? manLed : (alarm != ALARM_NONE);
    st.buzzer = manual ? manBuz : (alarm != ALARM_NONE);
    st.alarm  = alarm;
    if (forcePublish || st.manual != last.manual || st.led != last.led ||
        st.buzzer != last.buzzer || st.alarm != last.alarm) {
      queueSendLatest(statusQueue, &st);
      last = st;
      forcePublish = false;
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_ALERT, OUTPUT);
  pinMode(PIN_LED_NET, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED_ALERT, LOW);
  digitalWrite(PIN_LED_NET, LOW);

  serialMutex = xSemaphoreCreateMutex();
  netReadySem = xSemaphoreCreateBinary();
  dataQueue   = xQueueCreate(10, sizeof(SensorData_t));
  alertQueue  = xQueueCreate(5,  sizeof(AlertData_t));
  cmdQueue    = xQueueCreate(5,  sizeof(Command_t));
  statusQueue = xQueueCreate(3,  sizeof(Status_t));

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!bmp.begin()) {
    Serial.println("[BMP180] Khong tim thay cam bien! Kiem tra SDA/SCL.");
    while (true) delay(1000);
  }
  Serial.println("[BMP180] San sang");

  xTaskCreatePinnedToCore(taskSendData, "TaskSendData", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(taskReadData, "TaskReadData", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(taskAlert,    "TaskAlert",    4096, NULL, 3, NULL, 1);
}

void loop() {
  vTaskDelete(NULL);
}
