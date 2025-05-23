﻿import time
import re
import paho.mqtt.client as mqtt

# MQTT Broker details
BROKER = "192.168.12.227"
PORT = 1883
TOPIC_RECEIVE = "sensor/temperature"
TOPIC_ACK = "sensor/ack"

# Regex pattern to extract temperature from the message
temperature_pattern = re.compile(r'(\d+)\s*Temperature:\s*(-?\d+)°C')

def extract_temperature(data):
    """Extracts the temperature using Regex."""
    match = temperature_pattern.search(data)
    return match.group(2) if match else None

def clean_temperature(temperature):
    """Cleans the temperature string to avoid duplicates like '°C°C'."""
    return temperature.replace("°C°C", "°C")

def process_message(client, userdata, message):
    """Handles received MQTT message and sends ACK to QEMU."""
    try:
        data = message.payload.decode().strip()
        parts = data.split()

        if len(parts) >= 2:
            sent_time = parts[0]  # Timestamp from QEMU
            message_content = ' '.join(parts[1:])  # Message content (temperature only)

            temperature = None

            if "WARNING" in message_content:
                temperature = extract_temperature(message_content)
            else:
                temperature = message_content.split()[1]
                temperature = clean_temperature(temperature)

            print(f"Received: Temperature {temperature}")

            # Send ACK back to QEMU with same timestamp
            ack_message = f"{sent_time} ACK"
            client.publish(TOPIC_ACK, ack_message)
            print(f"Sent ACK to Qemu")

        else:
            print(f"Invalid data received: {data}")
    except Exception as e:
        print(f"Error processing message: {e}")

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"Connected to MQTT broker at {BROKER}:{PORT}")
        client.subscribe(TOPIC_RECEIVE)
    else:
        print(f"Failed to connect with result code {rc}")

def on_disconnect(client, userdata, rc):
    print(f"Disconnected with result code {rc}")

def start_mqtt_client():
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = process_message

    try:
        client.connect(BROKER, PORT, 60)
        print("MQTT client is running...")
        client.loop_forever()
    except Exception as e:
        print(f"Error starting MQTT client: {e}")
        print("Retrying in 5 seconds...")
        time.sleep(5)
        client.loop_forever()

if __name__ == "__main__":
    try:
        start_mqtt_client()
    except KeyboardInterrupt:
        print("\nServer was manually interrupted, shutting down...")