#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/misc/sensor_device.h"
#include "qapi/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#define TYPE_SENSOR_DEVICE "sensor_device"

pthread_t send_thread; // Thread for sending temperature

typedef struct SensorDeviceState SensorDeviceState;

DECLARE_INSTANCE_CHECKER(SensorDeviceState, SENSOR_DEVICE, TYPE_SENSOR_DEVICE)

// Register definition for the temperature sensor device
#define REG_TEMP 0x0 // Current temp reg
#define REG_MIN_TEMP 0x4 // Min temp reg
#define REG_MAX_TEMP 0x8 // Max temp reg
#define REG_STATUS 0xC // Sensor status reg
#define REG_CONTROL 0x10 // Control reg

#define SERVER_IP "192.168.12.227"
#define SERVER_PORT 8080

static const MemoryRegionOps sensor_device_ops;
static const TypeInfo sensor_device_info;

// Structure representing the sensor device state
struct SensorDeviceState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int temperature;
    int min_temp; // Min temp threshold
    int max_temp; // Max temp threshold
    int status;
    int is_sending; // Flag indicating whether data is being sent
    int sending_thread_created; // Flag indicating whether the sending thread is created
    int socket_fd; // Socket file descriptor for communication with Orange Pi
    int ack_received_recently; // Added member to store ACK status
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; // Mutex lock for thread synchronization

// Function Prototypes
void sensor_device_instance_init(Object* obj);
float generate_realistic_temperature(float current_temperature);
void send_temperature_to_orangepi(SensorDeviceState* s, int temperature);
void* send_temperature_thread(void* arg);
void sensor_device_register_types(void);
void stop_sending(SensorDeviceState* s);
void handle_ack(SensorDeviceState* s, char* message);

// Function to generate a realistic temperature based on the current temperature
float generate_realistic_temperature(float current_temperature) {
    float ambient_temp = 27.0; // Ambient temperature in °C
    float delta = ((rand() % 11) - 5) / 10.0; // Small variation between -0.5 and +0.5
    float sensor_noise = ((rand() % 21) - 10) / 100.0; // Noise ±0.1°C

    // Environmental effect on temperature to make changes more realistic
    float new_temperature = current_temperature + (ambient_temp - current_temperature) * 0.2 + delta + sensor_noise;

    // Ensure the temperature stays within realistic bounds
    if (new_temperature < 20) new_temperature = 20;
    if (new_temperature > 40) new_temperature = 40;

    return new_temperature;
}

// Function to read from the sensor device registers
static uint64_t sensor_device_read(void* opaque, hwaddr addr, unsigned int size) {
    SensorDeviceState* s = opaque;
    switch (addr) {
    case REG_TEMP:
        return s->temperature;
    case REG_MIN_TEMP:
        return s->min_temp;
    case REG_MAX_TEMP:
        return s->max_temp;
    case REG_CONTROL:
        return s->is_sending;
    case REG_STATUS:
        return s->status;
    default:
        printf("Unknown address: 0x%lx, returning 0xDEADBEEF\n", addr);
        return 0xDEADBEEF; // Return an error value for unknown addresses
    }
}

// Function to write to the sensor device registers
static void sensor_device_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size) {
    SensorDeviceState* s = opaque;
    switch (addr) {
    case REG_MIN_TEMP:
        s->min_temp = (int)value;
        printf("Min temperature set to %d°C\n", s->min_temp);
        break;
    case REG_MAX_TEMP:
        s->max_temp = (int)value;
        printf("Max temperature set to %d°C\n", s->max_temp);
        break;
    case REG_STATUS:
        s->status = (int)value;
        printf("Sensor status updated to %d\n", s->status);
        break;
    case REG_CONTROL:
        s->is_sending = (int)value;
        if (s->is_sending == 1) {
            if (s->sending_thread_created == 0) {
                // Create a thread to send temperature data
                pthread_create(&send_thread, NULL, send_temperature_thread, s);
                pthread_detach(send_thread);
                s->sending_thread_created = 1;
            }
        }
        else {
            if (s->sending_thread_created == 1) {
                s->sending_thread_created = 0;
                stop_sending(s); // Stop sending data
                printf("❌ Sending stopped.\n");
            }
        }
        break;
    default:
        printf("Unknown write address: 0x%lx, ignoring write\n", addr);
        break;
    }
}

// Function to send temperature data to the Orange Pi server
void send_temperature_to_orangepi(SensorDeviceState* s, int temperature) {
    static int sock = -1;
    struct sockaddr_in server_addr;
    char message[128];
    //struct timeval tv;
    struct timeval send_time, receive_time;  // لتعقب الوقت

    if (s->is_sending == 0) {
        return;
    }

    while (s->is_sending) {
        if (sock == -1) {
            int retry_count = 0;
            const int max_retries = 5;
            int backoff_time = 5; // Time to wait before retrying (in seconds)

            while (retry_count < max_retries) {
                if (s->is_sending == 0) {
                    return;
                }

                sock = socket(AF_INET, SOCK_STREAM, 0); // Create a socket
                if (sock < 0) {
                    perror("Socket creation failed");
                    return;
                }

                server_addr.sin_family = AF_INET;
                server_addr.sin_port = htons(SERVER_PORT);
                server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);

                if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                    printf("✅ Connected to Orange Pi!\n");
                    break;
                }

                perror("❌ Connection failed, retrying...");
                close(sock);
                sock = -1;

                // Sleep with increasing backoff time
                printf("Retrying in %d seconds...\n", backoff_time);
                sleep(backoff_time);
                backoff_time += 5;
                retry_count++;
            }

            if (retry_count == max_retries) {
                printf("⏳ Failed to connect after %d attempts. Sleeping for 5 minutes...\n", max_retries);
                sleep(300); // Wait for 5 minutes before retrying again
                continue;
            }
        }

        // Get the current timestamp in milliseconds
        gettimeofday(&send_time, NULL);  // Save the send time
        long timestamp_ms = send_time.tv_sec * 1000LL + send_time.tv_usec / 1000;

        int min_temp = s->min_temp;
        int max_temp = s->max_temp;

        // Check if the temperature is within the acceptable range
        if (temperature > max_temp) {
            snprintf(message, sizeof(message), "%ld WARNING: High Temperature %d °C", timestamp_ms, temperature);
            s->status = 1;
        }
        else if (temperature < min_temp) {
            snprintf(message, sizeof(message), "%ld WARNING: Low Temperature %d °C", timestamp_ms, temperature);
            s->status = -1;
        }
        else {
            snprintf(message, sizeof(message), "%ld Temperature %d °C", timestamp_ms, temperature);
            s->status = 0;
        }

        // Send the message to the server
        if (send(sock, message, strlen(message), 0) < 0) {
            printf("⚠️ Connection lost. Retrying...\n");
            close(sock);
            sock = -1;
            sleep(5); // Wait before attempting to reconnect
            continue;
        }

        printf("Sent: Temperature %d°C\n", temperature);

        // Wait for an ACK response
        fd_set readfds;
        struct timeval timeout;
        timeout.tv_sec = 2; // Set timeout of 2 seconds for ACK
        timeout.tv_usec = 0;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        int ret = select(sock + 1, &readfds, NULL, NULL, &timeout);
        if (ret > 0) {
            // Data is available, check if ACK is received
            char ack_message[128];
            int bytes_received = recv(sock, ack_message, sizeof(ack_message) - 1, 0);
            if (bytes_received > 0) {
                ack_message[bytes_received] = '\0'; // Null-terminate the string
                gettimeofday(&receive_time, NULL);  // Save the receive time
                long delay_ms = (receive_time.tv_sec - send_time.tv_sec) * 1000LL + (receive_time.tv_usec - send_time.tv_usec) / 1000;
                printf("Received ACK | Delay: %ld ms\n", delay_ms); // التعديل هنا
            }
            else {
                printf("No ACK received. Retrying...\n");
            }
        }
        else if (ret == 0) {
            // Timeout occurred, no ACK received
            printf("No ACK received...\n");
        }
        else {
            perror("Error in select");

        }

        sleep(5); // Send data every 5 seconds

        // Generate a new temperature reading for the next cycle
        temperature = generate_realistic_temperature(temperature);
    }

    if (s->is_sending == 0 && sock != -1) {
        close(sock);
        sock = -1;
        printf("❌ Connection closed.\n");
    }
}

// Handle ACK message and send temperature again if needed
void handle_ack(SensorDeviceState* s, char* message) {
    struct timeval tv;
    long now_ms = 0;
    long ack_timestamp = 0;

    gettimeofday(&tv, NULL);
    now_ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000;

    if (strncmp(message, "ACK", 3) == 0) {
        // إذا تم تلقي ACK، نقوم بحساب التأخير
        if (sscanf(message, "ACK %ld", &ack_timestamp) == 1) {
            long delay = now_ms - ack_timestamp;
            printf("ACK received | Delay: %ld ms\n", delay);
            s->ack_received_recently = 1;
        }
    }
    else {
        // إذا لم يتم تلقي ACK، نعرض الرسالة
        printf("No ACK received...\n");
        s->ack_received_recently = 0;
    }

    // إرسال درجة حرارة جديدة بعد تلقي ACK أو عدم تلقيه
    pthread_mutex_lock(&lock);
    s->temperature = generate_realistic_temperature(s->temperature); // توليد درجة حرارة جديدة
    pthread_mutex_unlock(&lock);
    send_temperature_to_orangepi(s, s->temperature); // إرسال الدرجة الجديدة
}

// Function to stop sending temperature data and close the connection
void stop_sending(SensorDeviceState* s) {
    if (s->is_sending == 0) {
        return;
    }

    // Stop sending data
    s->is_sending = 0;

    // Close the socket if it's open
    if (s->socket_fd != -1) {
        close(s->socket_fd);
        s->socket_fd = -1;
        printf("❌ Connection closed.\n");
    }
}

// Thread function to periodically send temperature data
void* send_temperature_thread(void* arg) {
    SensorDeviceState* s = (SensorDeviceState*)arg;

    while (1) {
        if (s->is_sending == 0) {
            stop_sending(s);
            break;
        }
        s->temperature = generate_realistic_temperature(s->temperature);
        send_temperature_to_orangepi(s, s->temperature);
        sleep(5);
    }

    return NULL;
}

// Initialize the sensor device instance
void sensor_device_instance_init(Object* obj) {
    SensorDeviceState* s = SENSOR_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &sensor_device_ops, s, TYPE_SENSOR_DEVICE, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);

    s->status = 0;
    s->is_sending = 0;
    s->sending_thread_created = 0;
    s->min_temp = 18;
    s->max_temp = 30;
    s->temperature = generate_realistic_temperature(s->temperature);
}

static const MemoryRegionOps sensor_device_ops = {
    .read = sensor_device_read,
    .write = sensor_device_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const TypeInfo sensor_device_info = {
    .name = TYPE_SENSOR_DEVICE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SensorDeviceState),
    .instance_init = sensor_device_instance_init,
};

// Register the sensor device types
void sensor_device_register_types(void) {
    type_register_static(&sensor_device_info);
}

type_init(sensor_device_register_types)

// Create a new sensor device instance
DeviceState* sensor_device_create(hwaddr addr) {
    DeviceState* dev = qdev_new(TYPE_SENSOR_DEVICE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}
