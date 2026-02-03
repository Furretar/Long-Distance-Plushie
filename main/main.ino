#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>  // for logging
#include <ArduinoJson.h>

const int motorPin = D10;

int startTime = 0;
int currentTime = 0;

// ms per network check
int networkCheck = 10000;

// out of 255
int default_strength = 150;
int strength = 0;

// default values
char* default_ssid = "ncsu";
char* default_password = "";

// mqtt
const char* MQTT_HOST = "lc600a99.ala.us-east-1.emqxsl.com";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "a";
const char* MQTT_PASS = "a";
const char* CMD_TOPIC = "devices/esp32_c3_1/cmd";
WiFiClientSecure wifi;
PubSubClient mqtt(wifi);

bool run_motor = false;
bool variables_set = false;

StaticJsonDocument<1024> doc;

#include <LittleFS.h>
#include <ArduinoJson.h>

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

void handle_command(String message) {
  message.trim();

  if (message.startsWith("run")) {
    int val = message.length() > 4 ? message.substring(4).toInt() : strength;
    if (val >= 0 && val <= 255) strength = val;
    run_motor = true;
    Serial.println("Motor will run at strength: " + String(strength));
  } else if (message.startsWith("strength")) {
    int val = message.length() > 9 ? message.substring(9).toInt() : strength;
    if (val >= 0 && val <= 255) {
      strength = val;
      set_strength_in_json(val);
      setup_print_config();
      Serial.println("Strength updated: " + String(strength));
    } else {
      Serial.println("Invalid strength value: " + String(val));
    }
  } else if (message.startsWith("add network")) {
    String params = message.substring(12);
    int spaceIndex = params.indexOf(' ');
    String ssid = spaceIndex > 0 ? params.substring(0, spaceIndex) : params;
    String password = spaceIndex > 0 ? params.substring(spaceIndex + 1) : "";
    add_network(ssid.c_str(), password.c_str());
  } else if (message.startsWith("delete network")) {
    String ssid = message.substring(15);
    delete_network(ssid.c_str());
  } else if (message.startsWith("delete")) {
    delete_config();
  } else if (message.startsWith("config")) {
    setup_print_config();
  } else {
    Serial.println("Invalid command: " + message);
  }
}


void connect_wifi() {
  WiFi.mode(WIFI_STA);
  delay(500);

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

  JsonArray networks = doc["networks"].as<JsonArray>();

  // backup network if empty
  if (!networks || networks.size() == 0) {
    Serial.print("no networks in config, trying default network: ");
    Serial.println(default_ssid);

    WiFi.begin(default_ssid, default_password);

    unsigned long startAttempt = millis();
    const unsigned long timeout = networkCheck;

    while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < timeout) {
      delay(500);
      unsigned long timePassed = millis() - startAttempt;
      Serial.println("Time passed: " + String(timePassed / 1000.0) + " seconds");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected to ");
      Serial.println(default_ssid);
      return;
    } else {
      Serial.println("Failed to connect to default network");
    }
  }
  // try every network in config
  else {
    for (JsonObject network : networks) {
      const char* ssid = network["ssid"];
      const char* password = network["password"];

      Serial.print("trying to connect to ");
      Serial.println(ssid);

      WiFi.begin(ssid, password);

      unsigned long startAttempt = millis();
      const unsigned long timeout = networkCheck;

      while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < timeout) {
        delay(500);
        unsigned long timePassed = millis() - startAttempt;
        Serial.println("Time passed: " + String(timePassed / 1000.0) + " seconds");
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Connected to ");
        Serial.println(ssid);
        return;
      } else {
        Serial.print("Failed to connect to ");
        Serial.println(ssid);
      }
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Could not connect to any network");
  }
}



// connect to mqtt
void connect_mqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // make mqtt use the callback function above when looping
  mqtt.setCallback(mqtt_callback);

  while (!mqtt.connected()) {
    Serial.println("connecting to mqtt");
    wifi.setInsecure();
    if (mqtt.connect("esp32_c3_1", MQTT_USER, MQTT_PASS)) {
      Serial.println("mqtt connected");
      mqtt.subscribe(CMD_TOPIC, 1);
    } else {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("Failed, rc=");
        Serial.print(mqtt.state());
        Serial.println(" try again in 2 seconds");
        delay(2000);
      } else {
        Serial.println("not connected to wifi, retrying");
        connect_wifi();
      }
    }
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
  pinMode(motorPin, OUTPUT);
  analogWrite(motorPin, 0);
  int startTime = millis();

  setup_print_config();
  connect_wifi();
  connect_mqtt();
}

unsigned long motorStart = 0;
const unsigned long motorDuration = 500;  // ms

// loop
void loop() {
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

  // avoids using delay, can react to messages instantly
  // checks if the motor is already running
  if (run_motor && motorStart == 0) {
    analogWrite(motorPin, strength);
    Serial.println("Motor Running at " + (String)((float)strength / 255 * 100) + "% strength, " + (String)strength);
    motorStart = millis();
    run_motor = false;
  }

  // motorStart > 0 | means currently running
  // millis() - motorStart >= motorDuration | turns off after motor duration
  if (motorStart > 0 && millis() - motorStart >= motorDuration) {
    analogWrite(motorPin, 0);
    Serial.println("Motor Off");
    motorStart = 0;
  }

  // read terminal commands
  if (Serial.available()) {
    String message = Serial.readStringUntil('\n');
    handle_command(message);
  }
}