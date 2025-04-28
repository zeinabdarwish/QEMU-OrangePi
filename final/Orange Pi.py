
import time
import random
import paho.mqtt.client as mqtt
import spidev
import re

# MQTT Broker details
BROKER = "192.168.12.227"
PORT = 1883
TOPIC_RECEIVE = "sensor/temperature"
TOPIC_ACK = "sensor/ack"

# Regex pattern to extract temperature from the message
temperature_pattern = re.compile(r'(\d+)\s*Temperature:\s*(-?\d+)°C')

# SPI Configuration
spi = spidev.SpiDev()
spi.open(1, 0)
spi.max_speed_hz = 500000
spi.mode = 0b00               # SPI Mode 0 (CPOL=0, CPHA=0)
spi.bits_per_word = 8         # 8-bit data
spi.lsbfirst = False          # MSB first
spi.cshigh = False            # Ensure CS is active low

def extract_temperature(data):
    """Extracts the temperature using Regex."""
    match = temperature_pattern.search(data)
    return match.group(2) if match else None

def clean_temperature(temperature):
    """Cleans the temperature string to avoid duplicates like '°C°C'."""
    return temperature.replace("°C°C", "°C").replace("°C", "")

def calculate_delay(start_time, end_time):
    """Calculates delay in milliseconds."""
    return (end_time - start_time) * 1000  # converting to milliseconds

def process_message(client, userdata, message):
    """Handles received MQTT message, sends to STM32 and sends ACK with delay."""
    try:
        data = message.payload.decode().strip()
        parts = data.split()

        if len(parts) >= 2:
            sent_time = parts[0]  # Timestamp from QEMU
            message_content = ' '.join(parts[1:])  # Message content (temperature)

            # Process temperature
            temperature = message_content.split()[1]  # Assuming the temperature is the second item
            temperature = clean_temperature(temperature)

            # Display received temperature
            print(f"Received: Temperature {temperature}")

            # If temperature is a valid integer, send it, otherwise ignore
            if temperature.isdigit():  # Only send if it's a valid integer
                to_send = int(temperature)
            else:
                print(f"Invalid temperature value: {temperature}. Skipping.")
                return  # Skip if the temperature value is not valid

            # Record the start time before sending data to STM32
            start_time = time.time()

            # Send the data to STM32 via SPI
            response = spi.xfer([to_send])[0]  # Send data to STM32 and receive the response

            # Record the end time after receiving the response
            end_time = time.time()

            # Calculate delay between Orange Pi and STM32
            delay = calculate_delay(start_time, end_time)

            # Print delay and response with correct label
            print(f"Sent: {to_send:2d}, Received: {hex(response).upper()[2:]}, Delay between Orange Pi and STM32: {delay:.3f} ms")

            # Send ACK back to QEMU with the same timestamp and delay
            ack_message = f"{sent_time} ACK | Delay: {delay:.3f} ms"
            client.publish(TOPIC_ACK, ack_message)

        else:
            print(f"Invalid data received: {data}")
    except Exception as e:
        print(f"Error processing message: {e}")

def on_connect(client, userdata, flags, rc):
    """Callback when MQTT client connects."""
    if rc == 0:
        print(f"Connected to MQTT broker at {BROKER}:{PORT}")
        client.subscribe(TOPIC_RECEIVE)
    else:
        print(f"Failed to connect with result code {rc}")

def on_disconnect(client, userdata, rc):
    """Callback when MQTT client disconnects."""
    print(f"Disconnected with result code {rc}")

def start_mqtt_client():
    """Start the MQTT client and handle message processing."""
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

# Run the program
if __name__ == "__main__":
    try:
        start_mqtt_client()
    except KeyboardInterrupt:
        print("\nServer was manually interrupted, shutting down...")
    finally:
        # Clean up SPI connection when done
        spi.close()
        print("SPI connection closed.")
