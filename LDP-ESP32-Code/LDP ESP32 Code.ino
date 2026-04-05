#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>  // for logging
#include <ArduinoJson.h>
#include "esp_sleep.h"  // for deep sleep
#include <time.h>

#define BATTERY_PIN D0
#define MOTOR_PIN D10
#define BUTTON_PIN D1  // GPIO 3, RTC capable/can wake up from deep sleep
#define RED_PIN D5
#define GREEN_PIN D4
#define BLUE_PIN D3

// change for each esp32
const int thisNum = 2;

const int targetNum = thisNum == 1 ? 2 : 1;
String thisTopic = String("esp32_") + String(thisNum);
String targetTopic = String("esp32_") + String(targetNum);
String thisPing = String("ping_") + String(thisNum);
String targetPing = String("ping_") + String(targetNum);

// change for other setups
const char* defaultMqttHost = "h862f16c.ala.us-east-1.emqxsl.com";
const int defaultMqttPort = 8883;
const char* defaultMqttUser = "a";
const char* defaultMqttPass = "a";
const char* defaultInfoTopic = "info";

// default config values
const int defaultCheckMqttTime = 100;               // ms, default 200, time awake to read retained mqtt messages
const int defaultStayAwakeAfterCommand = 120;       // seconds, default 120
const int defaultMotorTimeout = 10000;              // ms, default 10,000
const int defaultNormalCheck = 15ULL * 1000000ULL;  // microseconds, default 15,000,000, sleep for this long before waking up to check again
const int defaultSlowCheck = 30ULL * 1000000ULL;    // microseconds, default 30,000,000
const int defaultMaxPwm = 150;                      // out of 255, motor rated for 3 volts but powered by 3.7, default 150
const int defaultBrightness = 20;                   // percent max brightness
const int defaultStrength = 20;                     // percent of max motor strength, default 20
const int defaultTimeUntilSlowCheckMode = 2 * (60 * 60 * 24);

// config variables
RTC_DATA_ATTR int checkMqttTime;
RTC_DATA_ATTR unsigned long stayAwakeAfterCommand;
RTC_DATA_ATTR unsigned long motorTimeout;
RTC_DATA_ATTR unsigned long long normalCheck;
RTC_DATA_ATTR unsigned long long slowCheck;
RTC_DATA_ATTR int maxPwm;
RTC_DATA_ATTR int mqttPort;
RTC_DATA_ATTR char mqttHost[128];
RTC_DATA_ATTR char mqttUser[64];
RTC_DATA_ATTR char mqttPass[64];
RTC_DATA_ATTR char infoTopic[64];
RTC_DATA_ATTR int strength;
RTC_DATA_ATTR int brightness;
RTC_DATA_ATTR char default_ssid[32] = "";
RTC_DATA_ATTR char default_password[64] = "";
RTC_DATA_ATTR time_t timeUntilSlowCheckMode;

// non config rtc
RTC_DATA_ATTR bool debugMqttFailed = false;
RTC_DATA_ATTR bool firstBoot = true;
RTC_DATA_ATTR bool slowCheckMode = false;
RTC_DATA_ATTR time_t timeLastActive;

// global variables
bool offSwitch = false;
bool runMotor = false;
bool clearCommand = false;
bool clearPing = false;
bool variablesSet = false;
bool commandReceivedThisBoot = false;
bool otherAwake = false;
bool mqttConnected = false;
esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
volatile bool stayAwake = (wakeupReason == ESP_SLEEP_WAKEUP_GPIO);
int lastButtonState = HIGH;
unsigned long motorStart = 0;
unsigned long mqttConnectTime = 0;
StaticJsonDocument<1024> doc;
WiFiClientSecure wifi;
PubSubClient mqtt(wifi);


// led variables
int currentR = 0;
int currentG = 0;
int currentB = 0;
int bothAwakeR = 70;
int bothAwakeG = 70;
int bothAwakeB = 0;
int thisAwakeR = 0;
int thisAwakeG = 70;
int thisAwakeB = 0;

// sync variable
unsigned long lastPingSent = 0;
unsigned long lastPongReceived = 0;
const int pingInterval = 500;
const int pongTimeout = 2000;  // consider other ESP asleep after 2s

// track date
const char* ntpServer = "pool.ntp.org";
const long gmtOffsetSec = -5 * 3600;  // adjust timezone
RTC_DATA_ATTR time_t lastVoltageReport = 0;

void initTime() {
  configTime(gmtOffsetSec, 0, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
}

// lock pins when sleeping so leaking current doesn't cause led to glow
void goToSleep(unsigned long timeLength) {
  // force pin LOW so LED is off
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, LOW);

  // lock the pin state
  gpio_hold_en((gpio_num_t)RED_PIN);
  gpio_hold_en((gpio_num_t)GREEN_PIN);
  gpio_hold_en((gpio_num_t)BLUE_PIN);

  // report sleep
  String msg = "";
  if (timeLength > 0) {
    esp_sleep_enable_timer_wakeup(timeLength);
    msg = "sleeping for: " + String(timeLength / 1000000.0) + " seconds";

  } else {
    msg = "sleeping indefinitely";
  }

  Serial.println(msg);
  if (mqttConnected) {
    send_mqtt(infoTopic, msg);
    mqtt.loop();
    delay(50);
  }

  gpio_deep_sleep_hold_en();  // keep hold during deep sleep
  esp_deep_sleep_start();     // enter deep sleep
}


void set_led(int r, int g, int b) {
  currentR = r;
  currentG = g;
  currentB = b;

  r = min(255, int(r * brightness * 0.01));
  g = min(255, int(g * brightness * 0.01));
  b = min(255, int(b * brightness * 0.01));

  analogWrite(RED_PIN, r);
  analogWrite(GREEN_PIN, g);
  analogWrite(BLUE_PIN, b);
}

void led_off() {
  analogWrite(RED_PIN, 0);
  analogWrite(GREEN_PIN, 0);
  analogWrite(BLUE_PIN, 0);
}


void loadString(char* dst, size_t dstSize, JsonDocument& doc, const char* key, const char* def) {
  const char* src = doc[key] | def;
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}



// create config with default values
void setup_config() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  // wipe memory from doc
  doc.clear();

  // create file if it doesn't exist
  if (!LittleFS.exists("/networks.json")) {
    Serial.println("creating networks.json");
    File file = LittleFS.open("/networks.json", "w");

    // initialize keys
    JsonArray networks = doc.createNestedArray("networks");
    JsonObject defaultNetwork = networks.createNestedObject();
    defaultNetwork["ssid"] = "ncsu";
    defaultNetwork["password"] = "";
    doc["checkMqttTime"] = defaultCheckMqttTime;
    doc["stayAwakeAfterCommand"] = defaultStayAwakeAfterCommand;
    doc["motorTimeout"] = defaultMotorTimeout;
    doc["normalCheck"] = defaultNormalCheck;
    doc["slowCheck"] = defaultSlowCheck;
    doc["maxPwm"] = defaultMaxPwm;
    doc["mqttHost"] = defaultMqttHost;
    doc["mqttPort"] = defaultMqttPort;
    doc["mqttUser"] = defaultMqttUser;
    doc["mqttPass"] = defaultMqttPass;
    doc["infoTopic"] = defaultInfoTopic;
    doc["strength"] = defaultStrength;
    doc["brightness"] = defaultBrightness;
    doc["timeUntilSlowCheckMode"] = defaultTimeUntilSlowCheckMode;

    // write to file and close
    serializeJsonPretty(doc, file);
    file.close();
    Serial.println("networks.json created");
  }

  // open file for reading
  File file = LittleFS.open("/networks.json", "r");
  if (!file) {
    Serial.println("failed to open networks.json");
    return;
  }

  // check errors
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.print("Failed to parse networks.json: ");
    Serial.println(err.c_str());
    return;
  }

  // load vars from config to RTC memory
  load_config_vars();
}

void print_config_serial_mqtt() {
  Serial.println("networks.json:");
  serializeJsonPretty(doc, Serial);
  Serial.println();

  if (mqttConnected) {
    mqtt.setBufferSize(2048);
    String configMessage;
    serializeJsonPretty(doc, configMessage);
    send_mqtt(String(infoTopic), configMessage);
  }
}


// set global variables with config
void load_config_vars() {
  checkMqttTime = doc["checkMqttTime"];
  stayAwakeAfterCommand = doc["stayAwakeAfterCommand"];
  motorTimeout = doc["motorTimeout"];
  normalCheck = doc["normalCheck"];
  slowCheck = doc["slowCheck"];
  maxPwm = doc["maxPwm"];
  mqttPort = doc["mqttPort"];
  loadString(mqttUser, sizeof(mqttUser), doc, "mqttUser", defaultMqttUser);
  loadString(mqttPass, sizeof(mqttPass), doc, "mqttPass", defaultMqttPass);
  loadString(infoTopic, sizeof(infoTopic), doc, "infoTopic", defaultInfoTopic);
  loadString(mqttHost, sizeof(mqttHost), doc, "mqttHost", defaultMqttHost);
  strength = doc["strength"];
  brightness = doc["brightness"];
  timeUntilSlowCheckMode = doc["timeUntilSlowCheckMode"];
}

// send notification to info topic
void send_mqtt(String topic, String message) {
  message = String(thisTopic) + ": " + message;
  if (mqtt.publish(topic.c_str(), message.c_str())) {
  } else {
    Serial.println("Message failed to send.");
  }
}

// delete config
void delete_config() {
  if (LittleFS.exists("/networks.json")) {
    if (LittleFS.remove("/networks.json")) {
      Serial.println("networks.json deleted successfully");
    } else {
      Serial.println("Failed to delete networks.json");
      return;
    }
  } else {
    Serial.println("networks.json does not exist");
  }
  setup_config();
  print_config_serial_mqtt();
}


void add_network(const char* ssid, const char* password) {
  JsonArray networks = doc["networks"].as<JsonArray>();
  if (!networks) {
    networks = doc.createNestedArray("networks");
  }
  JsonObject net = networks.createNestedObject();
  net["ssid"] = ssid;
  net["password"] = password;

  File file = LittleFS.open("/networks.json", "w");
  serializeJson(doc, file);
  file.close();

  Serial.println("Network added: " + String(ssid));
  print_config_serial_mqtt();
}

void delete_network(const char* ssid) {
  JsonArray networks = doc["networks"].as<JsonArray>();
  if (!networks) return;
  for (int i = 0; i < networks.size(); i++) {
    JsonObject net = networks[i];
    if (net["ssid"] == ssid) {
      networks.remove(i);
      Serial.println("Network deleted: " + String(ssid));
      break;
    }
  }
  File file = LittleFS.open("/networks.json", "w");
  serializeJson(doc, file);
  file.close();

  print_config_serial_mqtt();
}


void set_config_value_int(const char* key, int new_value) {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  File file = LittleFS.open("/networks.json", "r");
  if (!file) {
    Serial.println("Failed to open networks.json for reading");
    return;
  }

  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.print("Failed to parse networks.json: ");
    Serial.println(err.c_str());
    return;
  }

  doc[key] = new_value;

  File fileOut = LittleFS.open("/networks.json", "w");
  if (!fileOut) {
    Serial.println("Failed to open networks.json for writing");
    return;
  }
  serializeJsonPretty(doc, fileOut);
  fileOut.close();

  Serial.print("Updated ");
  Serial.print(key);
  Serial.print(" to ");
  Serial.println(new_value);
}

void set_config_value_string(const char* key, const char* value) {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  File file = LittleFS.open("/networks.json", "r");
  if (!file) {
    Serial.println("Failed to open networks.json for reading");
    return;
  }

  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) {
    Serial.print("failed to parse networks.json: ");
    Serial.println(err.c_str());
    return;
  }

  doc[key] = value;

  File fileOut = LittleFS.open("/networks.json", "w");
  if (!fileOut) {
    Serial.println("failed to open networks.json for writing");
    return;
  }
  serializeJsonPretty(doc, fileOut);
  fileOut.close();
}


// reads and runs commands
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

  unsigned long messageDelay = millis() - mqttConnectTime;

  if (!commandReceivedThisBoot)
    Serial.println("Message arrived after: " + String(messageDelay) + " ms");

  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  handle_command(message);
}

// scans networks, for debugging
void scan_print_networks() {
  Serial.println("scanning available wifi networks...");
  int networksCount = WiFi.scanNetworks();

  while (networksCount == 0) {
    Serial.println("No networks found");
    networksCount = WiFi.scanNetworks();
  }

  Serial.println(String(networksCount) + " networks found");
  for (int i = 0; i < networksCount; ++i) {
    String ssid = WiFi.SSID(i);
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(ssid);
  }
}

void handle_command(String message) {
  message.trim();


  if (message.equals("")) {
    return;
  }

  // ignore pings when waking and sleeping normally
  bool isPingPong = (message == "ping" || message == "pong");
  if (isPingPong && !stayAwake) {
    return;
  }

  // keep device awake if a command arrives
  if (!isPingPong && !message.startsWith("var")) {
    time(&timeLastActive);
    commandReceivedThisBoot = true;
    stayAwake = true;
    Serial.println("received msg: " + message);
  }

  if (message.startsWith("run") && !runMotor) {
    int val = message.length() > 4 ? message.substring(4).toInt() : strength;
    if (val >= 0 && val <= 100) strength = val;

    int motorStrength = (val * maxPwm) / 100;

    runMotor = true;
    motorStart = millis();
    set_led(255, 255, 255);
    analogWrite(MOTOR_PIN, motorStrength);
    Serial.println("Motor ON (strength: " + String(val) + "%)");
  }

  else if (message.equals("stop")) {
    runMotor = false;
    motorStart = 0;
    analogWrite(MOTOR_PIN, 0);
    Serial.println("stopping");
  }

  else if (message.startsWith("config")) {
    String params = message.substring(7);
    params.trim();

    int firstSpace = params.indexOf(' ');
    if (firstSpace < 0) {
      Serial.println("usage: config <key> <value>");
      return;
    }

    String key = params.substring(0, firstSpace);
    String valueStr = params.substring(firstSpace + 1);
    valueStr.trim();

    // list of integer keys
    const char* intKeys[] = {
      "strength", "maxPwm", "motorTimeout",
      "checkMqttTime", "stayAwakeAfterCommand", "normalCheck", "slowCheck", "brightness", nullptr
    };
    // list of string keys
    const char* stringKeys[] = {
      "mqttHost", "mqttPort", "mqttUser", "mqttPass", "infoTopic", nullptr
    };
    bool handled = false;

    // check if it's an integer key
    for (int i = 0; intKeys[i] != nullptr; i++) {
      if (key == intKeys[i]) {
        int valueInt = valueStr.toInt();
        set_config_value_int(key.c_str(), valueInt);
        handled = true;
        break;
      }
    }

    // check if it's a string key
    if (!handled) {
      for (int i = 0; stringKeys[i] != nullptr; i++) {
        if (key == stringKeys[i]) {
          set_config_value_string(key.c_str(), valueStr.c_str());
          handled = true;
          break;
        }
      }
    }
    if (!handled) {
      Serial.println("Unknown config key: " + key);
    }
    print_config_serial_mqtt();
  }

  else if (message.startsWith("add network")) {
    String params = message.substring(12);
    int spaceIndex = params.indexOf(' ');
    String ssid = spaceIndex > 0 ? params.substring(0, spaceIndex) : params;
    String password = spaceIndex > 0 ? params.substring(spaceIndex + 1) : "";
    add_network(ssid.c_str(), password.c_str());
  }

  else if (message.startsWith("delete network")) {
    String ssid = message.substring(15);
    delete_network(ssid.c_str());
  }

  else if (message.equals("delete config")) {
    delete_config();
  }

  else if (message.startsWith("print config")) {
    print_config_serial_mqtt();
  }

  else if (message.startsWith("print voltage")) {
    read_and_print_voltage();
  }

  else if (message.equals("off")) {
    // clear retained message or it'll reread and sleep forever
    mqtt.publish(thisTopic.c_str(), "", true);
    goToSleep(0);
  }

  else if (message.startsWith("sleep")) {
    // clear retained message or it'll reread and sleep forever
    mqtt.publish(thisTopic.c_str(), "", true);
    goToSleep(normalCheck);
  }

  else if (message.equals("ping")) {
    mqtt.publish(targetPing.c_str(), "pong");
  }

  else if (message.equals("pong")) {
    lastPongReceived = millis();
    otherAwake = true;
  }

  else if (message == "var all") {
    String msg = "----- VARIABLES -----\n";

    msg += "offSwitch: " + String(offSwitch) + "\n";
    msg += "runMotor: " + String(runMotor) + "\n";
    msg += "clearCommand: " + String(clearCommand) + "\n";
    msg += "clearPing: " + String(clearPing) + "\n";
    msg += "variablesSet: " + String(variablesSet) + "\n";
    msg += "commandReceivedThisBoot: " + String(commandReceivedThisBoot) + "\n";
    msg += "lastButtonState: " + String(lastButtonState) + "\n";

    msg += "motorStart: " + String(motorStart) + "\n";
    msg += "mqttConnectTime: " + String(mqttConnectTime) + "\n";

    msg += "stayAwake: " + String(stayAwake) + "\n";
    msg += "otherAwake: " + String(otherAwake) + "\n";
    msg += "mqttConnected: " + String(mqttConnected) + "\n";

    msg += "lastPingSent: " + String(lastPingSent) + "\n";
    msg += "lastPongReceived: " + String(lastPongReceived) + "\n";

    msg += "pingInterval: " + String(pingInterval) + "\n";
    msg += "pongTimeout: " + String(pongTimeout) + "\n";

    msg += "debugMqttFailed: " + String(debugMqttFailed) + "\n";
    msg += "firstBoot: " + String(firstBoot) + "\n";
    msg += "slowCheckMode: " + String(slowCheckMode) + "\n";

    // convert timeLastActive to readable format
    time_t adjustedTime = timeLastActive + gmtOffsetSec;
    struct tm timeinfo;
    if (localtime_r(&adjustedTime, &timeinfo)) {
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
      msg += "timeLastActive: " + String(buffer) + "\n";
    } else {
      msg += "timeLastActive: invalid\n";
    }

    Serial.println(msg);
    if (mqttConnected) {
      mqtt.setBufferSize(2048);
      send_mqtt(infoTopic, msg);
      mqtt.loop();
      delay(50);
    }
  }

  else {
    Serial.println("Invalid command: " + message);
  }

  if (!isPingPong) {
    clearCommand = true;
    ;
  }
}


String connect_wifi() {

  // turn led on if device is active
  if (stayAwake) {
    set_led(255, 0, 0);
  }

  const int maxTotalRetries = 3;
  const int lastNetworkTries = 2;
  int networkCheckTimeout = 5000;

  WiFi.mode(WIFI_STA);

  // reduce driver overhead
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);  // increases power consumption but decreases connection times

  JsonArray networks = doc["networks"].as<JsonArray>();

  unsigned long wifiStart = millis();
  for (int attempt = 0; attempt < maxTotalRetries; attempt++) {

    // try to connect to last connected network first
    if (strlen(default_ssid) > 0) {
      for (int attempt = 0; attempt < lastNetworkTries; attempt++) {
        // ensure clean stack
        Serial.println("Connecting to last network, attempt " + String(attempt + 1) + " of " + String(lastNetworkTries) + ": " + String(default_ssid));

        WiFi.begin(default_ssid, default_password);
        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < networkCheckTimeout) {
          yield();
        }

        if (WiFi.status() == WL_CONNECTED) {
          Serial.println("WiFi connected to last network in " + String((millis() - startAttempt) / 1000.0) + " seconds");
          return default_ssid;
        }

        Serial.println("Failed attempt " + String(attempt) + " for last network");
      }
    }

    // try every network in config
    for (JsonObject network : networks) {
      const char* ssid = network["ssid"];
      const char* password = network["password"];

      Serial.println("trying network in config: " + String(ssid));
      unsigned long netStart = millis();
      WiFi.begin(ssid, password);

      while (WiFi.status() != WL_CONNECTED && millis() - netStart < networkCheckTimeout) {
        yield();
      }

      unsigned long netTime = millis() - netStart;
      Serial.println("Attempt took " + String(netTime) + " ms");

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected to: " + String(ssid) + " in " + String((millis() - netStart) / 1000.0) + " seconds");

        strncpy(default_ssid, ssid, sizeof(default_ssid) - 1);
        default_ssid[sizeof(default_ssid) - 1] = '\0';
        strncpy(default_password, password, sizeof(default_password) - 1);
        default_password[sizeof(default_password) - 1] = '\0';

        // move the connected network to the front of the list
        for (int i = 0; i < networks.size(); i++) {
          if (networks[i]["ssid"] == ssid) {
            if (i != 0) {  // only move if not already at front

              // copy the network
              String foundSsid = networks[i]["ssid"].as<String>();
              String foundPass = networks[i]["password"].as<String>();
              networks.remove(i);

              // shift elements to the right
              networks.add(JsonObject());  // add a placeholder at the end
              for (int j = networks.size() - 1; j > 0; j--) {
                networks[j]["ssid"] = networks[j - 1]["ssid"].as<String>();
                networks[j]["password"] = networks[j - 1]["password"].as<String>();
              }
              networks[0]["ssid"] = foundSsid;
              networks[0]["password"] = foundPass;

              // write changes to file
              File file = LittleFS.open("/networks.json", "w");
              serializeJson(doc, file);
              file.close();
            }
            break;
          }
        }

        unsigned long totalTime = millis() - wifiStart;
        Serial.println("Total time to try all networks: " + String(totalTime) + " ms");

        return ssid;
      }

      Serial.println("failed: " + String(ssid));
    }

    if (attempt < maxTotalRetries - 1) {
      Serial.println("Retrying WiFi...");
    }
  }

  Serial.println("could not connect to any network");
  Serial.println("Total time for all attempts: " + String(millis() - wifiStart) + " ms");
  return "";
}

// connect to mqtt
bool connect_mqtt() {

  // led feedback if button press
  if (stayAwake) {
    set_led(0, 0, 255);
  }

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (mqtt.connected()) {
    return true;
  }

  wifi.setInsecure();
  mqtt.setServer(mqttHost, mqttPort);
  mqtt.setCallback(mqtt_callback);

  Serial.println("attempting mqtt connection...");
  unsigned long startAttempt = millis();
  const int maxTotalRetries = 2;
  int retries = 0;

  while (!mqtt.connected() && retries < maxTotalRetries) {

    String clientId = String(thisTopic) + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str(), mqttUser, mqttPass)) {
      Serial.println("mqtt connected in " + String((millis() - startAttempt) / 1000.0) + " seconds");
      mqttConnectTime = millis();
      mqtt.subscribe(thisTopic.c_str(), 1);
      mqtt.subscribe(thisPing.c_str(), 0);
      return true;
    }

    set_led(0, 0, 255);
    retries++;
  }

  Serial.println("mqtt failed (rc=" + String(mqtt.state()) + ")");
  return false;
}


void read_and_print_voltage() {
  uint32_t sum_mV = 0;

  for (int i = 0; i < 16; i++) {
    sum_mV += analogReadMilliVolts(BATTERY_PIN);
  }

  float adcVoltage = sum_mV / 16.0 / 1000.0;  // average and convert to volts
  float batteryVolts = adcVoltage * 2.0;      // factor = (R1 + R2)/R2, 2 for 220k:220k

  String msg = "Battery voltage: " + String(batteryVolts, 3) + " V";
  Serial.print("Battery voltage: ");
  Serial.print(batteryVolts, 3);
  Serial.println(" V");
  send_mqtt(String(infoTopic), msg);
}

// interrupt wifi and mqtt connection for led feedback
String ssid = "";
void IRAM_ATTR buttonISR() {
  if (!stayAwake) {
    stayAwake = true;

    // probably shouldn't use set_led in an ISR trigger, but works fine so far
    // really important for feedback, LED stays blank when connecting otherwise
    if (ssid.length() == 0) {
      set_led(255, 0, 0);
    } else {
      set_led(0, 0, 255);
    }
  }
}

// setup
void setup() {
  Serial.begin(115200);
  Serial.println("-------------------------");
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // motor
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(MOTOR_PIN, OUTPUT);
  analogWrite(MOTOR_PIN, 0);

  // led
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  // release hold on leds
  gpio_hold_dis((gpio_num_t)RED_PIN);
  gpio_hold_dis((gpio_num_t)GREEN_PIN);
  gpio_hold_dis((gpio_num_t)BLUE_PIN);

  // interrupt wifi and mqtt connection for led feedback
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  if (stayAwake) {
    set_led(255, 0, 0);
  }

  // wake up from button or timer
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

  // keep awake if woken by button
  if (stayAwake) {
    Serial.println("woke up from button press");
    time(&timeLastActive);
    commandReceivedThisBoot = true;
    set_led(255, 0, 0);
  }

  if (!variablesSet) {
    variablesSet = true;
  }

  setup_config();

  // connect wifi and mqtt
  ssid = connect_wifi();
  WiFi.setSleep(true);  // setsleep reduces power usage, but increases connection time, only enable after connected
  mqttConnected = connect_mqtt();
  mqttConnectTime = millis();

  lastPongReceived = millis();
  led_off();

  if (stayAwake) {
    time(&timeLastActive);
  }

  // short window to read serial commands
  unsigned long serialWindow = 50;
  unsigned long start = millis();
  while (millis() - start < serialWindow) {
    if (Serial.available()) {
      String message = Serial.readStringUntil('\n');
      handle_command(message);
    }
  }

  // sleep longer if mqtt keeps failing to save battery
  if (!mqttConnected) {
    debugMqttFailed = true;
    Serial.println("MQTT or wifi failed, going to sleep");
    Serial.flush();
    goToSleep(slowCheck * 2);
  }

  // send debug message to show that mqtt failed
  if (debugMqttFailed) {
    debugMqttFailed = false;
    send_mqtt(infoTopic, "mqtt or wifi failed");
  }

  // send voltage report or enter slow check mode
  time_t now;
  time(&now);

  if (firstBoot) {

    initTime();
    time(&now);
    time(&timeLastActive);

    // print config to serial
    Serial.println("networks.json:");
    serializeJsonPretty(doc, Serial);
    Serial.println();

    // send voltage report on first boot
    if (mqttConnected) {
      read_and_print_voltage();
      lastVoltageReport = now;
    }

    firstBoot = false;

  } else {

    // enter slow check mode
    initTime();
    time(&now);
    // Serial.println("now=" + String(now) + " timeLastActive=" + String(timeLastActive) + " diff=" + String(difftime(now, timeLastActive)));
    if (now - timeLastActive > timeUntilSlowCheckMode) {
      Serial.println("entering slow check mode");
      slowCheckMode = true;
    } else {
      if (slowCheckMode) {
        Serial.println("leaving slow check mode");
        slowCheckMode = false;
      }
    }


    // send voltage after interval passes

    const int sendVoltageInterval = 3600;  // send voltage to info topic at this interval, default 3600/1 hour
    if (lastVoltageReport == 0 || difftime(now, lastVoltageReport) > sendVoltageInterval) {
      initTime();
      time(&now);
      read_and_print_voltage();
      lastVoltageReport = now;
    }
  }
}



// loop
void loop() {

  int buttonState = digitalRead(BUTTON_PIN);

  // keep device active while button is held
  if (buttonState == LOW) {
    time(&timeLastActive);
  }

  // button pressed
  if (buttonState == LOW && lastButtonState == HIGH) {
    stayAwake = true;
    if (otherAwake) {
      set_led(255, 0, 255);
    } else {
      set_led(0, 0, 255);
    }

    time(&timeLastActive);
    commandReceivedThisBoot = true;
    Serial.println("sending run to other esp32");
    mqtt.publish(targetTopic.c_str(), "run", true);
  }
  // button released, stop motor
  if (buttonState == HIGH && lastButtonState == LOW) {
    stayAwake = true;
    time(&timeLastActive);
    commandReceivedThisBoot = true;
    Serial.println("sending stop to other esp32");
    mqtt.publish(targetTopic.c_str(), "stop", true);
  }

  // clear mqtt messages
  if (clearCommand) {
    mqtt.publish(thisTopic.c_str(), "", true);
    clearCommand = false;
  }
  if (clearPing) {
    mqtt.publish(thisPing.c_str(), "", true);
    clearPing = false;
  }

  lastButtonState = buttonState;
  mqtt.loop();

  // motor timeout
  if (runMotor && millis() - motorStart > motorTimeout) {
    runMotor = false;
    led_off();
    analogWrite(MOTOR_PIN, 0);
    Serial.println("Motor timed out");
  }

  // read terminal commands
  if (Serial.available()) {
    String message = Serial.readStringUntil('\n');
    handle_command(message);
  }

  // wait for retained messages to arrive
  static unsigned long loopStartTime = 0;
  if (loopStartTime == 0) {
    loopStartTime = millis();
  }

  time_t now;
  time(&now);
  unsigned long timeSincemqtt = millis() - mqttConnectTime;
  double timeSinceCommand = difftime(now, timeLastActive);

  // sleep if not running motor
  if (!runMotor && timeSincemqtt > checkMqttTime && (!stayAwake || timeSinceCommand > stayAwakeAfterCommand)) {
    Serial.flush();
    if (!slowCheckMode) {
      goToSleep(normalCheck);
    } else {
      goToSleep(slowCheck);
    }
  }

  if (stayAwake && millis() - lastPingSent > pingInterval) {
    lastPingSent = millis();
    mqtt.publish(targetPing.c_str(), "ping");
  }

  if (millis() - lastPongReceived > pongTimeout) {
    otherAwake = false;
  }

  static unsigned long lastLedUpdate = 0;
  if (millis() - lastLedUpdate > 100) {
    lastLedUpdate = millis();

    if (!runMotor && buttonState == HIGH && stayAwake) {
      if (!otherAwake) {
        set_led(thisAwakeR, thisAwakeG, thisAwakeB);
      } else {
        set_led(bothAwakeR, bothAwakeG, bothAwakeB);
      }
    }
  }
}
