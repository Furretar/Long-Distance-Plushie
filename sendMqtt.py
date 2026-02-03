import paho.mqtt.client as mqtt
import threading
import time
import random
import ssl

# might change
TOPIC = "esp32_c3_1"

# EMQX Connection Details
BROKER = "lc600a99.ala.us-east-1.emqxsl.com"
PORT = 8883  # TLS/SSL port
USERNAME = "a"  # EMQX username
PASSWORD = "a"  # EMQX password


def on_connect(client, userdata, flags, rc):
    print(f"Connected with result code {rc}")
    if rc == 0:
        print("✓ Connection successful!")
    else:
        print(f"✗ Connection failed!")
        print(f"  Code {rc}: {mqtt.connack_string(rc)}")

def on_disconnect(client, userdata, rc):
    if rc != 0:
        print(f"⚠ Unexpected disconnection. Code: {rc}")

def on_publish(client, userdata, mid):
    print(f"  → Message delivered (mid: {mid})")

def on_log(client, userdata, level, buf):
    print(f"[LOG] {buf}")

client_id = f"python_controller_{random.randint(1000, 9999)}"
print(f"Client ID: {client_id}")

client = mqtt.Client(client_id=client_id, protocol=mqtt.MQTTv311)

# Enable logging to see what's happening
client.on_log = on_log

# Set username and password for EMQX
client.username_pw_set(USERNAME, PASSWORD)

# Enable TLS but skip certificate verification (like your ESP32 does with setInsecure)
client.tls_set(cert_reqs=ssl.CERT_NONE)
client.tls_insecure_set(True)

client.on_connect = on_connect
client.on_disconnect = on_disconnect
client.on_publish = on_publish

print(f"Connecting to {BROKER}:{PORT}...")
try:
    client.connect(BROKER, PORT, keepalive=60)
except Exception as e:
    print(f"✗ Connection error: {e}")
    exit(1)

client.loop_start()
time.sleep(2)

def send_commands():
    while True:
        cmd = input("\nEnter command (or 'quit'): ").strip()
        if cmd.lower() == 'quit':
            break
        if cmd:
            print(f"Publishing '{cmd}' to {TOPIC}...")
            info = client.publish(TOPIC, cmd, qos=1, retain=False)
            info.wait_for_publish()
            if info.is_published():
                print(f"✓ Published successfully!")
            else:
                print(f"✗ Publish failed!")

print("\n" + "="*50)
print("MQTT Controller Ready")
print("="*50)
threading.Thread(target=send_commands, daemon=True).start()

try:
    while True:
        time.sleep(1)
except KeyboardInterrupt:
    print("\n\nExiting...")
    client.loop_stop()
    client.disconnect()