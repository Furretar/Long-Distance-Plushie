const int motorPin = D10;

// out of 255
const int strength = 20;

void setup() {
  Serial.begin(115200);
  pinMode(motorPin, OUTPUT);
}

void loop() {
  analogWrite(motorPin, strength); 
  Serial.println("Motor Running at Half Strength");
  delay(500);

  // off
  analogWrite(motorPin, 0);
  Serial.println("Motor Off");
  delay(1000);
}