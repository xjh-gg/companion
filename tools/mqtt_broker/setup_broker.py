import json
import os
import secrets
import subprocess
import sys

ROOT = r"D:\Projects\workspace\companion"
BROKER_DIR = os.path.join(ROOT, "tools", "mqtt_broker")
MOSQ_DIR = r"C:\Program Files (x86)\mosquitto"
SOURCE = r"C:\Users\x1773\AppData\Local\Temp\xiaozhi_src\xiaozhi-esp32-2.0.3"
HOST = "192.168.137.1"
PORT = 1884

os.makedirs(BROKER_DIR, exist_ok=True)
passwd = os.path.join(BROKER_DIR, "passwd")
acl = os.path.join(BROKER_DIR, "acl")
conf = os.path.join(BROKER_DIR, "mosquitto.conf")
secrets_path = os.path.join(BROKER_DIR, ".secrets.json")
header_path = os.path.join(SOURCE, "main", "companion", "companion_mqtt_business_config.h")

device_pw = secrets.token_urlsafe(18)
cloud_pw = secrets.token_urlsafe(18)

mosq_passwd = os.path.join(MOSQ_DIR, "mosquitto_passwd.exe")
subprocess.run([mosq_passwd, "-c", "-b", passwd, "companion-device", device_pw], check=True)
subprocess.run([mosq_passwd, "-b", passwd, "companion-cloud", cloud_pw], check=True)

with open(acl, "w", encoding="utf-8") as f:
    f.write(
        "user companion-device\n"
        "topic write companion/+/event\n"
        "topic write companion/+/status\n"
        "topic read companion/+/command\n\n"
        "user companion-cloud\n"
        "topic read companion/+/event\n"
        "topic read companion/+/status\n"
        "topic write companion/+/command\n"
    )

with open(conf, "w", encoding="utf-8") as f:
    f.write(
        f"listener {PORT} 0.0.0.0\n"
        "allow_anonymous false\n"
        f"password_file {passwd.replace('\\', '/')}\n"
        f"acl_file {acl.replace('\\', '/')}\n"
        "persistence false\n"
        f"log_dest file {os.path.join(BROKER_DIR, 'mosquitto.log').replace('\\', '/')}\n"
    )

with open(secrets_path, "w", encoding="utf-8") as f:
    json.dump(
        {
            "host": HOST,
            "port": PORT,
            "device": {"username": "companion-device", "password": device_pw},
            "cloud": {"username": "companion-cloud", "password": cloud_pw},
        },
        f,
        indent=2,
    )

os.makedirs(os.path.dirname(header_path), exist_ok=True)
with open(header_path, "w", encoding="utf-8") as f:
    f.write(
        "#pragma once\n"
        f'#define COMPANION_BUSINESS_MQTT_ENDPOINT "mqtt://{HOST}:{PORT}"\n'
        '#define COMPANION_BUSINESS_MQTT_USERNAME "companion-device"\n'
        f'#define COMPANION_BUSINESS_MQTT_PASSWORD "{device_pw}"\n'
    )

print("BROKER_CONFIG", conf)
print("SECRETS", secrets_path)
print("DEVICE_CONFIG", header_path)
print("BROKER", f"{HOST}:{PORT}")
