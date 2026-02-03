#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

const int motorPin = D10;

int startTime = 0;
int currentTime = 0;


// out of 255
int strength = 150;

// wifi login
const char* whotspot_ssid = "whotspot";
const char* whotspot_pass = "deez1234";

// ncsu details
const char* ncsu_ssid = "ncsu";
const char* ncsu_pass = "";

// mqtt
const char* MQTT_HOST = "lc600a99.ala.us-east-1.emqxsl.com";
const int MQTT_PORT = 8883;
const char* MQTT_USER = "a";  // token
const char* MQTT_PASS = "a";
const char* CMD_TOPIC = "devices/esp32_c3_1/cmd";
WiFiClientSecure wifi;
PubSubClient mqtt(wifi);
bool run_motor = false;


// reads command on topic
void mqtt_callback(char* topic, byte* payload, unsigned int length) {

  // print out every message
  Serial.print("Message arrived on topic: ");
  Serial.println(topic);
  Serial.print("Payload: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();
  Serial.print("Payload bytes: ");
  for (unsigned int i = 0; i < length; i++) {
    Serial.print(payload[i]);
    Serial.print(" ");
  }
  Serial.println();

  // set run motor if command is RUN
  if (length >= 3 && payload[0] == 'R' && payload[1] == 'U' && payload[2] == 'N') {

    // check for number after RUN to set strength to
    if (length > 3) {
      char buf[8];
      int n = min((int)length - 3, 7);
      memcpy(buf, payload + 3, n);
      buf[n] = '\0';

      int val = atoi(buf);
      if (val >= 0 && val <= 255) {
        strength = val;
      }
    }

    run_motor = true;
  }
}

// connects to wifi
void connect_wifi() {
  // sets wifi to station mode, acts as client (instead of access point/host)
  WiFi.mode(WIFI_STA);
  delay(500);

  // print available wifi networks
  Serial.println("scanning available wifi networks...");
  int networksCount = WiFi.scanNetworks();
  currentTime = millis();
  Serial.println("scanned networks: " + (String)(currentTime - startTime));
  bool eduroamFound = false;
  bool ncsuFound = false;
  bool whotspotFound = false;

  while (networksCount == 0) {
    Serial.println("No networks found");
    networksCount = WiFi.scanNetworks();
  }

  Serial.println((String)(networksCount) + " networks found");
  for (int i = 0; i < networksCount; ++i) {
    String ssid = WiFi.SSID(i);
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(ssid);
    Serial.println();

    // check for ncsu
    if (ssid.equals(ncsu_ssid)) {
      ncsuFound = true;
    }
    // check for whotspot
    if (ssid.equals(whotspot_ssid)) {
      whotspotFound = true;
    }
  }

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // try whotspot
  if (whotspotFound) {
    Serial.println("connecting to whotspot");
    WiFi.begin(whotspot_ssid, whotspot_pass);
  }
  // try ncsu
  else if (ncsuFound) {
    Serial.println("connecting to ncsu");
    WiFi.begin(ncsu_ssid, ncsu_pass);
  }
  // else
  else {
    Serial.println("not ncsu guest nor whotspot");
    WiFi.begin(whotspot_ssid, whotspot_pass);
  }

  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("attempting to connect: seconds " + String(counter));
    counter++;
  }
  currentTime = millis();
  Serial.println("logged in: " + (String)(currentTime - startTime));
  Serial.println("\nWi-Fi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
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
      Serial.print("Failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" try again in 2 seconds");
      delay(2000);
    }
  }
}


// setup
void setup() {
  Serial.begin(115200);
  pinMode(motorPin, OUTPUT);
  analogWrite(motorPin, 0);
  int startTime = millis();



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
    Serial.println("Motor Running at " + (String)((float)strength / 255 * 100) + "% strength");
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
}