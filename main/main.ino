#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>  // for logging
#include <ArduinoJson.h>
#include "esp_sleep.h"  // for deep sleep

#define MOTOR_PIN D10
#define BUTTON_PIN D2  // GPIO 4, RTC capable/can wake up from deep sleep

//// configurable default values
// change for each esp32
const char* thisTopic = "esp32_1";
const char* targetTopic = "esp32_2";

// change for other setups
const char* default_mqtt_host = "lc600a99.ala.us-east-1.emqxsl.com";
const int default_mqtt_port = 8883;
const char* default_mqtt_user = "a";
const char* default_mqtt_pass = "a";
const char* default_info_topic = "info";
char default_ssid[32] = "ncsu";
char default_password[64] = "";

// default config values
const int default_check_mqtt_time = 200;             // ms, default 200, time awake to read retained mqtt messages
const int default_stay_awake_after_command = 60000;  // ms, default 60,000
const int default_motor_timeout = 3000;              // ms, default 10,000
const int default_normal_check = 5ULL * 1000000ULL;  // microseconds, default 15,000,000, sleep for this long before waking up to check again
const int default_max_pwm = 150;                     // out of 255, motor rated for 3 volts but powered by 3.7, default 150
const int default_strength = 20;

// global RTC persistent/config vars
RTC_DATA_ATTR int check_mqtt_time;
RTC_DATA_ATTR unsigned long stay_awake_after_command;
RTC_DATA_ATTR unsigned long motor_timeout;
RTC_DATA_ATTR unsigned long long normal_check;
RTC_DATA_ATTR int max_pwm;
RTC_DATA_ATTR int mqtt_port;
RTC_DATA_ATTR char mqtt_host[128];
RTC_DATA_ATTR char mqtt_user[64];
RTC_DATA_ATTR char mqtt_pass[64];
RTC_DATA_ATTR char info_topic[64];
RTC_DATA_ATTR int strength;
RTC_DATA_ATTR bool no_sleep;  // for debugging



// global vars
bool off_switch = false;

bool run_motor = false;
bool clear_message = false;
bool variables_set = false;
bool commandReceivedThisBoot = false;

StaticJsonDocument<1024> doc;
int networkCheckTimeout = 2000;  // timeout in ms
unsigned long motorStart = 0;

unsigned long lastCommandTime = 0;
unsigned long mqttConnectTime = 0;
WiFiClientSecure wifi;
PubSubClient mqtt(wifi);



void loadString(char* dst, size_t dstSize, JsonDocument& doc, const char* key, const char* def) {
  const char* src = doc[key] | def;
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}



// create config with default values
void setup_print_config() {
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
    defaultNetwork["ssid"] = default_ssid;
    defaultNetwork["password"] = default_password;
    doc["check_mqtt_time"] = default_check_mqtt_time;
    doc["stay_awake_after_command"] = default_stay_awake_after_command;
    doc["motor_timeout"] = default_motor_timeout;
    doc["normal_check"] = default_normal_check;
    doc["max_pwm"] = default_max_pwm;
    doc["mqtt_host"] = default_mqtt_host;
    doc["mqtt_port"] = default_mqtt_port;
    doc["mqtt_user"] = default_mqtt_user;
    doc["mqtt_pass"] = default_mqtt_pass;
    doc["info_topic"] = default_info_topic;
    doc["strength"] = default_strength;

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

  // print config
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
  max_pwm = doc["max_pwm"];
  mqtt_port = doc["mqtt_port"];
  loadString(mqtt_user, sizeof(mqtt_user), doc, "mqtt_user", default_mqtt_user);
  loadString(mqtt_pass, sizeof(mqtt_pass), doc, "mqtt_pass", default_mqtt_pass);
  loadString(info_topic, sizeof(info_topic), doc, "info_topic", default_info_topic);
  loadString(mqtt_host, sizeof(mqtt_host), doc, "mqtt_host", default_mqtt_host);
  strength = doc["strength"];
}

// send notification to info topic
void send_info(String message, String topic) {
  message = String(thisTopic) + ": " + message;
  if (mqtt.publish(info_topic, message.c_str())) {
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

  setup_print_config();
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
  setup_print_config();
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
  setup_print_config();
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

  Serial.println("resetting last command time");
  lastCommandTime = millis();
  commandReceivedThisBoot = true;

  if (message.startsWith("run") && !run_motor) {
    int val = message.length() > 4 ? message.substring(4).toInt() : strength;
    if (val >= 0 && val <= 100) strength = val;

    int motorStrength = (val * max_pwm) / 100;

    run_motor = true;
    motorStart = millis();
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
      "check_mqtt_time", "stay_awake_after_command", "normal_check", nullptr
    };
    // list of string keys
    const char* stringKeys[] = {
      "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass", "info_topic", nullptr
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
    setup_print_config();
  }

  else if (message.equals("sleep on")) {
    no_sleep = false;
  }

  else if (message.equals("sleep off")) {
    no_sleep = true;
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
    setup_print_config();
  }

  else {
    Serial.println("Invalid command");
  }

  clear_message = true;
}


String connect_wifi() {
  const int maxRetries = 5;
  const unsigned long retryDelay = 2000;
  WiFi.mode(WIFI_STA);
  JsonArray networks = doc["networks"].as<JsonArray>();

  for (int attempt = 0; attempt < maxRetries; attempt++) {

    // try to connect to last connected network first
    if (strlen(default_ssid) > 0) {
      Serial.println("connecting to last network: " + String(default_ssid));

      WiFi.disconnect(true);
      delay(100);  // short pause to let driver reset
      WiFi.begin(default_ssid, default_password);

      unsigned long startAttempt = millis();

      while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < networkCheckTimeout) {
        yield();
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi connected in " + String((millis() - startAttempt) / 1000.0) + " seconds");
        return default_ssid;
      }

      Serial.println("Failed to connect to last network");
    }

    // try every network in config
    for (JsonObject network : networks) {
      const char* ssid = network["ssid"];
      const char* password = network["password"];
      Serial.println("trying network in config: " + String(ssid));

      WiFi.disconnect(true);
      delay(100);  // short pause to let driver reset
      WiFi.begin(ssid, password);

      unsigned long startAttempt = millis();
      const unsigned long timeout = networkCheckTimeout;

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

  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (mqtt.connected()) {
    return true;
  }

  wifi.setInsecure();
  mqtt.setServer(mqtt_host, mqtt_port);

  // make mqtt use the callback function above when looping
  mqtt.setCallback(mqtt_callback);

  Serial.println("connecting to mqtt");

  int retries = 0;

  while (!mqtt.connected() && retries < 5) {
    unsigned long startAttempt = millis();

    if (mqtt.connect("esp32_1", mqtt_user, mqtt_pass)) {
      Serial.println("mqtt connected in " + String((millis() - startAttempt) / 1000.0) + " seconds");
      mqttConnectTime = millis();
      mqtt.subscribe(thisTopic, 1);
      return true;
    }

    Serial.println("mqtt failed (rc=" + String(mqtt.state()) + ")");

    delay(1000);
    retries++;
  }

  Serial.println("mqtt connect failed after retries");
  return false;
}










// setup
void setup() {
  Serial.begin(115200);
  Serial.println("-------------------------");
  pinMode(BUTTON_PIN, INPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(MOTOR_PIN, OUTPUT);
  analogWrite(MOTOR_PIN, 0);
  setup_print_config();

  if (!variables_set) {
    no_sleep = false;
    variables_set = true;
  }

  lastCommandTime = 0;

  // connect wifi and mqtt
  String ssid = connect_wifi();
  bool mqtt_connected = connect_mqtt();

  esp_sleep_enable_timer_wakeup(normal_check);
}

// button vars
int lastButtonState = LOW;

// loop
void loop() {
  int buttonState = digitalRead(BUTTON_PIN);
  if (buttonState == HIGH && lastButtonState == LOW && run_motor == false) {
    mqtt.publish(thisTopic, "run");
  }
  // button released, stop motor
  if (buttonState == LOW && lastButtonState == HIGH && run_motor == true) {
    mqtt.publish(thisTopic, "stop");
  }

  if (clear_message) {
    Serial.println("clearing topic");
    mqtt.publish(thisTopic, "", true);
    clear_message = false;
  }
  lastButtonState = buttonState;

  mqtt.loop();

  // motor timeout
  if (run_motor && millis() - motorStart > motor_timeout) {
    run_motor = false;
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
  if (!run_motor && !no_sleep && timeSincemqtt > check_mqtt_time && (!commandReceivedThisBoot || timeSinceCommand > stay_awake_after_command)) {
    Serial.println("sleeping for: " + String(normal_check / 1000000.0) + " seconds");
    esp_deep_sleep_start();
  }
}
