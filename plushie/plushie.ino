#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>  // for logging
#include <ArduinoJson.h>
#include "esp_sleep.h"  // for deep sleep
#include <time.h>

#define BATTERY_PIN D0
#define MOTOR_PIN D10
#define BUTTON_PIN D2  // GPIO 4, RTC capable/can wake up from deep sleep
#define BUTTON_GPIO 4
#define RED_PIN D5
#define GREEN_PIN D4
#define BLUE_PIN D3

//// configurable default values
// change for each esp32
const char* thisNum = "2";
const char* targetNum = "1";

String thisTopic = String("esp32_") + thisNum;
String targetTopic = String("esp32_") + targetNum;
String thisPing = String("ping_") + thisNum;
String targetPing = String("ping_") + targetNum;

// change for other setups
const char* default_mqtt_host = "h862f16c.ala.us-east-1.emqxsl.com";
const int default_mqtt_port = 8883;
const char* default_mqtt_user = "a";
const char* default_mqtt_pass = "a";
const char* default_infoTopic = "info";

// default config values
const int default_check_mqtt_time = 200;              // ms, default 200, time awake to read retained mqtt messages
const int default_stay_awake_after_command = 120000;  // ms, default 120,000
const int default_motor_timeout = 10000;              // ms, default 10,000
const int default_normal_check = 15ULL * 1000000ULL;  // microseconds, default 15,000,000, sleep for this long before waking up to check again
const int default_slow_check = 30ULL * 1000000ULL;    // microseconds, default 30,000,000
const int default_max_pwm = 150;                      // out of 255, motor rated for 3 volts but powered by 3.7, default 150
const int default_brightness = 20;                    // percent max brightness
const int default_strength = 20;                      // percent of max motor strength, default 20

//// global RTC persistent
//config vars
RTC_DATA_ATTR int check_mqtt_time;
RTC_DATA_ATTR unsigned long stay_awake_after_command;
RTC_DATA_ATTR unsigned long motor_timeout;
RTC_DATA_ATTR unsigned long long normal_check;
RTC_DATA_ATTR unsigned long long slow_check;
RTC_DATA_ATTR int max_pwm;
RTC_DATA_ATTR int mqtt_port;
RTC_DATA_ATTR char mqtt_host[128];
RTC_DATA_ATTR char mqtt_user[64];
RTC_DATA_ATTR char mqtt_pass[64];
RTC_DATA_ATTR char infoTopic[64];
RTC_DATA_ATTR int strength;
RTC_DATA_ATTR int brightness;
RTC_DATA_ATTR char default_ssid[32] = "";
RTC_DATA_ATTR char default_password[64] = "";
RTC_DATA_ATTR bool firstBoot = true;
RTC_DATA_ATTR bool debugMqttFailed = false;

// global vars
bool off_switch = false;
bool run_motor = false;
bool clear_command = false;
bool clear_ping = false;
bool variables_set = false;
bool commandReceivedThisBoot = false;
int lastButtonState = HIGH;
StaticJsonDocument<1024> doc;
int networkCheckTimeout = 10000;  // time spent trying to connect to each wifi network
unsigned long motorStart = 0;
unsigned long lastCommandTime = 0;
unsigned long mqttConnectTime = 0;
WiFiClientSecure wifi;
PubSubClient mqtt(wifi);
esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
volatile bool stayAwake = (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO);
bool otherAwake = false;

// led variables
int current_r = 0;
int current_g = 0;
int current_b = 0;
int both_awake_r = 70;
int both_awake_g = 70;
int both_awake_b = 0;
int this_awake_r = 0;
int this_awake_g = 70;
int this_awake_b = 0;

// sync variable
unsigned long lastPingSent = 0;
unsigned long lastPongReceived = 0;
const int pingInterval = 500;
const int pongTimeout = 2000;  // consider other ESP asleep after 2s


// track date for printing voltage
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 3600;  // adjust timezone
RTC_DATA_ATTR int lastReportedDay = -1;

void initTime() {
  configTime(gmtOffset_sec, 0, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
}

// lock pins when sleeping so leaking current doesn't cause led to glow
void goToSleep() {
  // force pin LOW so LED is off
  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);
  digitalWrite(BLUE_PIN, LOW);

  // lock the pin state
  gpio_hold_en((gpio_num_t)RED_PIN);
  gpio_hold_en((gpio_num_t)GREEN_PIN);
  gpio_hold_en((gpio_num_t)BLUE_PIN);

  gpio_deep_sleep_hold_en();  // keep hold during deep sleep
  esp_deep_sleep_start();     // enter deep sleep
}


void set_led(int r, int g, int b) {
  current_r = r;
  current_g = g;
  current_b = b;

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
    doc["check_mqtt_time"] = default_check_mqtt_time;
    doc["stay_awake_after_command"] = default_stay_awake_after_command;
    doc["motor_timeout"] = default_motor_timeout;
    doc["normal_check"] = default_normal_check;
    doc["slow_check"] = default_slow_check;
    doc["max_pwm"] = default_max_pwm;
    doc["mqtt_host"] = default_mqtt_host;
    doc["mqtt_port"] = default_mqtt_port;
    doc["mqtt_user"] = default_mqtt_user;
    doc["mqtt_pass"] = default_mqtt_pass;
    doc["infoTopic"] = default_infoTopic;
    doc["strength"] = default_strength;
    doc["brightness"] = default_brightness;

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

void print_config() {
  Serial.println("networks.json:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
}


// set global variables with config
void load_config_vars() {
  check_mqtt_time = doc["check_mqtt_time"];
  stay_awake_after_command = doc["stay_awake_after_command"];
  motor_timeout = doc["motor_timeout"];
  normal_check = doc["normal_check"];
  slow_check = doc["slow_check"];
  max_pwm = doc["max_pwm"];
  mqtt_port = doc["mqtt_port"];
  loadString(mqtt_user, sizeof(mqtt_user), doc, "mqtt_user", default_mqtt_user);
  loadString(mqtt_pass, sizeof(mqtt_pass), doc, "mqtt_pass", default_mqtt_pass);
  loadString(infoTopic, sizeof(infoTopic), doc, "infoTopic", default_infoTopic);
  loadString(mqtt_host, sizeof(mqtt_host), doc, "mqtt_host", default_mqtt_host);
  strength = doc["strength"];
  brightness = doc["brightness"];
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
  Serial.println("got here");

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

  print_config();
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
  print_config();
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

  print_config();
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

  // keep device awake if a normal command arrives
  if (!isPingPong) {
    Serial.println("resetting last command time, message: " + message);
    lastCommandTime = millis();
    commandReceivedThisBoot = true;
    stayAwake = true;
  }

  if (message.startsWith("run") && !run_motor) {
    int val = message.length() > 4 ? message.substring(4).toInt() : strength;
    if (val >= 0 && val <= 100) strength = val;

    int motorStrength = (val * max_pwm) / 100;

    run_motor = true;
    motorStart = millis();
    set_led(255, 255, 255);
    analogWrite(MOTOR_PIN, motorStrength);
    Serial.println("Motor ON (strength: " + String(val) + "%)");
  }

  else if (message.equals("stop")) {
    run_motor = false;
    motorStart = 0;
    analogWrite(MOTOR_PIN, 0);
    Serial.println("stopping");
  }

  else if (message.equals("off")) {
    off_switch = true;
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
      "strength", "max_pwm", "motor_timeout",
      "check_mqtt_time", "stay_awake_after_command", "normal_check", "slow_check", "brightness", nullptr
    };
    // list of string keys
    const char* stringKeys[] = {
      "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass", "infoTopic", nullptr
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
    print_config();
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
    print_config();
  }

  else if (message.startsWith("print voltage")) {
    read_and_print_voltage();
  }

  else if (message.startsWith("sleep")) {
    // clear retained message or it'll reread and sleep forever
    mqtt.publish(thisTopic.c_str(), "", true);
    delay(100);

    esp_sleep_enable_timer_wakeup(normal_check);
    goToSleep();
  }

  else if (message.equals("ping")) {
    mqtt.publish(targetPing.c_str(), "pong");
  }

  else if (message.equals("pong")) {
    lastPongReceived = millis();
    otherAwake = true;
  }

  else {
    Serial.println("Invalid command: " + message);
  }

  if (!isPingPong) {
    clear_command = true;
    ;
  }
}


String connect_wifi() {

  if (stayAwake) {
    set_led(255, 0, 0);
  }

  const int maxRetries = 3;
  const unsigned long retryDelay = 1000;
  WiFi.mode(WIFI_STA);
  JsonArray networks = doc["networks"].as<JsonArray>();

  for (int attempt = 0; attempt < maxRetries; attempt++) {

    // try to connect to last connected network first
    if (strlen(default_ssid) > 0) {
      for (int attempt = 1; attempt <= 2; attempt++) {
        if (attempt == 1) WiFi.disconnect(true);
        Serial.println("Connecting to last network, attempt " + String(attempt) + ": " + String(default_ssid));

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
        delay(retryDelay);
      }
    }

    // try every network in config
    for (JsonObject network : networks) {
      const char* ssid = network["ssid"];
      const char* password = network["password"];
      Serial.println("trying network in config: " + String(ssid));

      WiFi.disconnect(true);
      delay(100);
      WiFi.begin(ssid, password);

      unsigned long startAttempt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < networkCheckTimeout) {
        yield();
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected to: " + String(ssid) + " in " + String((millis() - startAttempt) / 1000.0) + " seconds");

        strncpy(default_ssid, ssid, sizeof(default_ssid) - 1);
        default_ssid[sizeof(default_ssid) - 1] = '\0';
        strncpy(default_password, password, sizeof(default_password) - 1);
        default_password[sizeof(default_password) - 1] = '\0';

        return ssid;
      }

      Serial.println("failed: " + String(ssid));
    }

    if (attempt < maxRetries - 1) {
      Serial.println("Retrying WiFi in " + String(retryDelay / 1000) + " seconds...");
      delay(retryDelay);
    }
  }

  Serial.println("could not connect to any network");
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
  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setCallback(mqtt_callback);

  Serial.println("attempting mqtt connection...");
  unsigned long startAttempt = millis();
  const int maxRetries = 2;
  int retries = 0;

  while (!mqtt.connected() && retries < maxRetries) {

    String clientId = String(thisTopic) + String(random(0xffff), HEX);
    if (mqtt.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
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
bool mqtt_connected = false;
void IRAM_ATTR buttonISR() {
  if (!stayAwake) {
    stayAwake = true;
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
  esp_deep_sleep_enable_gpio_wakeup(1 << BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);

  // keep awake if woken by button
  if (stayAwake) {
    Serial.println("Woke up from button press");
    lastCommandTime = millis();
    commandReceivedThisBoot = true;
    set_led(255, 0, 0);
  }

  if (!variables_set) {
    variables_set = true;
  }

  if (!stayAwake) {
    lastCommandTime = 0;
  }

  setup_config();

  // connect wifi and mqtt
  ssid = connect_wifi();
  mqtt_connected = connect_mqtt();
  lastPongReceived = millis();
  led_off();

  if (stayAwake) {
    lastCommandTime = millis();
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
  if (!mqtt_connected) {
    debugMqttFailed = true;
    Serial.println("MQTT connect failed, going to deep sleep");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(slow_check * 2);
    goToSleep();
  }

  // send debug message to show that mqtt failed
  if (debugMqttFailed) {
    debugMqttFailed = false;
    mqtt.publish(infoTopic, "mqtt failed");
  }

  // send config to info topic
  if (firstBoot) {
    if (mqtt_connected) {
      read_and_print_voltage();

      struct tm timeinfo;
      if (getLocalTime(&timeinfo)) {
        lastReportedDay = timeinfo.tm_mday;
      }
    }

    mqtt.setBufferSize(2048);
    String configMessage;
    serializeJsonPretty(doc, configMessage);
    send_mqtt(String(infoTopic), configMessage);
    firstBoot = false;
  } else {
    // send voltage to info topic once a day
    initTime();
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    int currentDay = timeinfo.tm_mday;
    if (lastReportedDay != currentDay) {
      read_and_print_voltage();
      lastReportedDay = currentDay;
    }
  }
}



// loop
void loop() {

  int buttonState = digitalRead(BUTTON_PIN);

  // button pressed
  if (buttonState == LOW && lastButtonState == HIGH) {
    stayAwake = true;
    if (otherAwake) {
      set_led(255, 0, 255);
    } else {
      set_led(0, 0, 255);
    }

    lastCommandTime = millis();
    commandReceivedThisBoot = true;
    Serial.println("sending run to other esp32");
    mqtt.publish(targetTopic.c_str(), "run", true);
  }
  // button released, stop motor
  if (buttonState == HIGH && lastButtonState == LOW) {
    stayAwake = true;
    lastCommandTime = millis();
    commandReceivedThisBoot = true;
    Serial.println("sending stop to other esp32");
    mqtt.publish(targetTopic.c_str(), "stop", true);
  }

  // clear mqtt messages
  if (clear_command) {
    mqtt.publish(thisTopic.c_str(), "", true);
    clear_command = false;
  }
  if (clear_ping) {
    mqtt.publish(thisPing.c_str(), "", true);
    clear_ping = false;
  }

  lastButtonState = buttonState;
  mqtt.loop();

  // motor timeout
  if (run_motor && millis() - motorStart > motor_timeout) {
    run_motor = false;
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

  unsigned long timeSincemqtt = millis() - mqttConnectTime;
  unsigned long timeSinceCommand = millis() - lastCommandTime;

  // sleep if not running motor
  if (!run_motor && timeSincemqtt > check_mqtt_time && (!stayAwake || timeSinceCommand > stay_awake_after_command)) {
    Serial.println("sleeping for: " + String(normal_check / 1000000.0) + " seconds");
    send_mqtt(thisPing, "sleep");
    Serial.flush();
    esp_sleep_enable_timer_wakeup(normal_check);
    goToSleep();
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

    if (!run_motor && buttonState == HIGH && stayAwake) {
      if (!otherAwake) {
        set_led(this_awake_r, this_awake_g, this_awake_b);
      } else {
        set_led(both_awake_r, both_awake_g, both_awake_b);
      }
    }
  }
}
