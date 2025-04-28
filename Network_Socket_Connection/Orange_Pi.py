import socket
import time
import threading

SERVER_IP = "192.168.12.227"
SERVER_PORT = 8080

def handle_client(conn, addr):
    """Handles client connection, receives data, and sends ACK to QEMU."""
    print(f"Connection from {addr} established.")
    try:
        while True:
            data = conn.recv(1024).decode().strip()
            if not data:
                break

            parts = data.split()

            if len(parts) >= 2:
                try:
                    sent_time = parts[0]  # Time when the data was sent
                    message = ' '.join(parts[1:])  # The rest is the message (temperature)

                    # Extract temperature
                    temperature = message.split()[1]  # Extract temperature value

                    # Display the temperature
                    print(f"Received: Temperature {temperature}°C")

                    # Send ACK to QEMU with the same sent_time
                    ack_message = f"{sent_time} ACK"
                    # إرسال ACK إلى QEMU
                    print(f"Sent ACK to Qemu")

                    # إرسال الـ ACK عبر الشبكة
                    conn.send(ack_message.encode('utf-8'))

                except ValueError:
                    print(f"Invalid data format: {data}")
            else:
                print(f"Invalid data received: {data}")

    except Exception as e:
        print(f"Error with {addr}: {e}")
    finally:
        print(f"Connection with {addr} closed.")
        conn.close()

def start_server():
    """Starts the server and listens for incoming connections."""
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((SERVER_IP, SERVER_PORT))
    server_socket.listen(5)
    print(f"Server listening on {SERVER_IP}:{SERVER_PORT}...")

    while True:
        try:
            conn, addr = server_socket.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
        except Exception as e:
            print(f"Server error: {e}")

if __name__ == "__main__":
    try:
        start_server()
    except KeyboardInterrupt:
        print("\nServer was manually interrupted, shutting down...")
