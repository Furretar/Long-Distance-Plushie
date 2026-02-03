#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <esp_wpa2.h>  // to connect to eduroam
#include <HTTPClient.h> // for ncsu-guest captive portal

const int motorPin = D10;

int startTime = 0;
int currentTime = 0;


// out of 255
int strength = 20;

// wifi login
const char* whotspot_ssid = "whotspot";
const char* whotspot_pass = "deez1234";

// eduroam details
const char* eduroam_ssid = "eduroama";
const char* edu_identity = "wjkalise@ncsu.edu";
const char* edu_pass = "KrustyKrab3!";

// ncsu guest details
const char* ncsuguest_ssid = "ncsu-guesta";
const char* ncsuguest_pass = "";

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
  currentTime = millis();
  Serial.println("starting to connect to wifi: " + (String)(currentTime - startTime));

  // sets wifi to station mode, acts as client (instead of access point/host)
  WiFi.mode(WIFI_STA);
  currentTime = millis();
  Serial.println("set wifi mode: " + (String)(currentTime - startTime));

  // print available wifi networks
  Serial.println("scanning available wifi networks...");
  int n = WiFi.scanNetworks();
  currentTime = millis();
  Serial.println("scanned networks: " + (String)(currentTime - startTime));
  bool eduroamFound = false;
  bool ncsuguestFound = false;
  bool whotspotFound = false;

  if (n == 0) {
    Serial.println("No networks found");
  } else {
    Serial.println((String)n + " networks found");
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(ssid);
      Serial.println();

      // check for eduroam
      if (ssid.equals(eduroam_ssid)) {
        eduroamFound = true;
      }
      // check for ncsuguest
      if (ssid.equals(ncsuguest_ssid)) {
        ncsuguestFound = true;
      }
      // check for whotspot
      if (ssid.equals(whotspot_ssid)) {
        whotspotFound = true;
      }
    }
  }

  if (eduroamFound) {
    // log in with eduroam wpa2
    Serial.println("✅ eduroam network detected!");

    esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)edu_identity, strlen(edu_identity));
    esp_wifi_sta_wpa2_ent_set_username((uint8_t*)edu_identity, strlen(edu_identity));
    esp_wifi_sta_wpa2_ent_set_password((uint8_t*)edu_pass, strlen(edu_pass));
    esp_wifi_sta_wpa2_ent_enable();

    WiFi.begin(eduroam_ssid);

    Serial.println("Connecting to eduroam...");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }

    Serial.println("\nConnected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  }
  else {
    // log in normally

    // try whotspot
    if (whotspotFound) {
      Serial.println("connecting to whotspot");
      WiFi.begin(whotspot_ssid, whotspot_pass);
    }
    // try ncsuguest
    else if (ncsuguestFound) {
      Serial.println("connecting to ncsu guest");
      WiFi.begin(ncsuguest_ssid, ncsuguest_pass);
    } else {
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
}

void authenticate_captive_portal() {
  return;
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
    Serial.println("Motor Running at Half Strength");
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