#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>  // for logging
#include <ArduinoJson.h>

#define SLEEP_TIME 5
#define MOTOR_PIN D10
#define BUTTON_PIN D8

// change for each esp32
const char* thisTopic = "esp32_1";
const char* targetTopic = "esp32_2";


// RTC persistent config vars
RTC_DATA_ATTR int strength = 0;
RTC_DATA_ATTR char default_ssid[32] = "ncsu";
RTC_DATA_ATTR char default_password[64] = "";
const unsigned long motorTimeout = 500;  // ms
const char* MQTT_HOST = "lc600a99.ala.us-east-1.emqxsl.com";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "a";
const char* MQTT_PASS = "a";
const char* info_topic = "info";

// global vars
unsigned long motorStart = 0;
bool run_motor = false;
bool variables_set = false;
StaticJsonDocument<1024> doc;
int networkCheckTimeout = 5000;  // timeout in ms
bool offSwitch = false;


WiFiClientSecure wifi;
PubSubClient mqtt(wifi);



// send message
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

void set_strength_in_json(int new_strength) {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }

  // read existing json
  File file = LittleFS.open("/networks.json", "r");
  if (!file) {
    Serial.println("Failed to open networks.json for reading");
    return;
  }

  file.close();

  doc["strength"] = new_strength;

  File fileOut = LittleFS.open("/networks.json", "w");
  if (!fileOut) {
    Serial.println("Failed to open networks.json for writing");
    return;
  }
  serializeJsonPretty(doc, fileOut);
  fileOut.close();

  Serial.print("Updated strength to ");
  Serial.println(new_strength);
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

  if (message.startsWith("run")) {
    int val = message.length() > 4 ? message.substring(4).toInt() : strength;
    if (val >= 0 && val <= 255) strength = val;

    run_motor = true;
    motorStart = millis();
    analogWrite(MOTOR_PIN, strength);
    Serial.println("Motor ON (strength: " + String(strength) + ")");
  }

  else if (message.startsWith("stop")) {
    run_motor = false;
    motorStart = 0;
    analogWrite(MOTOR_PIN, 0);
    Serial.println("stopping");
  }

  else if (message.startsWith("off")) {
    offSwitch = true;
  }

  else if (message.startsWith("strength")) {
    int val = message.length() > 9 ? message.substring(9).toInt() : strength;
    if (val >= 0 && val <= 255) {
      strength = val;
      set_strength_in_json(val);
      setup_print_config();
      Serial.println("Strength updated: " + String(strength));
    }

    else {
      Serial.println("Invalid strength value: " + String(val));
    }
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

  else if (message.startsWith("delete")) {
    delete_config();
  }

  else if (message.startsWith("config")) {
    setup_print_config();
  }

  else {
    Serial.println((String)(thisTopic) + ": " + message);
  }
}


String connect_wifi() {
  WiFi.mode(WIFI_STA);

  // scan and print networks, for debugging
  // unsigned long scanStart = millis();
  // scan_print_networks();
  // Serial.println("networks scan: " + String((millis() - scanStart) / 1000.0) + " seconds");

  JsonArray networks = doc["networks"].as<JsonArray>();

  // try to connect to last connected network first
  if (strlen(default_ssid) > 0) {
    Serial.println("connecting to last network: " + String(default_ssid));

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
    return "";
  }
  // try every network in config
  for (JsonObject network : networks) {
    const char* ssid = network["ssid"];
    const char* password = network["password"];
    Serial.println("trying network in config: " + String(ssid));

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

  Serial.println("could not connect to any network");
  return "";
}

// connect to mqtt
void connect_mqtt() {
  wifi.setInsecure();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // make mqtt use the callback function above when looping
  mqtt.setCallback(mqtt_callback);

  Serial.println("connecting to mqtt");


  int retries = 0;

  while (!mqtt.connected() && retries < 5) {
    unsigned long startAttempt = millis();
    if (mqtt.connect("esp32_1", MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT connected in " + String((millis() - startAttempt) / 1000.0) + " seconds");
      mqtt.subscribe(thisTopic, 1);
      break;
    }

    Serial.print("MQTT failed (rc=");
    Serial.print(mqtt.state());
    Serial.println("), retrying in 5 seconds");

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      connect_wifi();
    }

    delay(5000);
    retries++;
  }
  if (!mqtt.connected()) {
    Serial.println("MQTT connect failed after retries");
  }
}


// config
void setup_print_config() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  doc.clear();

  // create file if it doesn't exist
  if (!LittleFS.exists("/networks.json")) {
    Serial.println("creating networks.json");
    File file = LittleFS.open("/networks.json", "w");

    doc["strength"] = 150;
    JsonArray networks = doc.createNestedArray("networks");
    JsonObject defaultNetwork = networks.createNestedObject();
    defaultNetwork["ssid"] = default_ssid;
    defaultNetwork["password"] = default_password;

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

  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.print("Failed to parse networks.json: ");
    Serial.println(err.c_str());
    return;
  }

  if (!variables_set) {
    strength = doc["strength"] | 150;
    variables_set = true;
  }

  // print config
  Serial.println("networks.json:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
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
  String ssid = connect_wifi();
  connect_mqtt();
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


  lastButtonState = buttonState;

  if (!mqtt.connected()) {
    unsigned long now = millis();
    static unsigned long lastReconnect = 0;
    if (now - lastReconnect > 5000) {
      lastReconnect = now;
      Serial.println("MQTT disconnected! Reconnecting...");
      connect_mqtt();
    }
  }

  mqtt.loop();

  // timeout motor
  unsigned long now = millis();
  if (motorStart > 0 && (now - motorStart) > motorTimeout) {
    run_motor = false;
    motorStart = 0;
    analogWrite(MOTOR_PIN, 0);
    Serial.println("Motor timed out");
  }

  // read terminal commands
  if (Serial.available()) {
    String message = Serial.readStringUntil('\n');
    handle_command(message);
  }
}