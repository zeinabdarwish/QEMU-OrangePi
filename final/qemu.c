#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/misc/sensor_device.h"
#include "qapi/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>
#include <mosquitto.h>
#include <errno.h>

#define TYPE_SENSOR_DEVICE "sensor_device"
#define MQTT_BROKER "192.168.12.227"
#define MQTT_PORT 1883

#define MQTT_TOPIC "sensor/temperature"
#define REG_TEMP 0x0
#define REG_MIN_TEMP 0x4
#define REG_MAX_TEMP 0x8
#define REG_STATUS 0xC
#define REG_CONTROL 0x10

typedef struct SensorDeviceState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    int temperature;
    int min_temp;
    int max_temp;
    int status;
    int is_sending;
    struct mosquitto* mosq;
    pthread_t send_thread;
    pthread_t receive_thread;
    long last_sent_timestamp;
    int ack_received_recently;
} SensorDeviceState;

typedef struct SensorDeviceState SensorDeviceState;
DECLARE_INSTANCE_CHECKER(SensorDeviceState, SENSOR_DEVICE, TYPE_SENSOR_DEVICE)

void* receive_temperature_thread(void* arg);
float generate_realistic_temperature(float current_temperature);
void send_temperature_via_mqtt(SensorDeviceState* s, int temperature);
void* send_temperature_thread(void* arg);
void sensor_device_instance_init(Object* obj);
void sensor_device_register_types(void);
void on_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message);

static const MemoryRegionOps sensor_device_ops;
static const TypeInfo sensor_device_info;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

float generate_realistic_temperature(float current_temperature) {
    float ambient_temp = 27.0;
    float delta = ((rand() % 11) - 5) / 10.0;
    float sensor_noise = ((rand() % 21) - 10) / 100.0;
    float new_temperature = current_temperature + (ambient_temp - current_temperature) * 0.2 + delta + sensor_noise;
    return (new_temperature < 20) ? 20 : (new_temperature > 40) ? 40 : new_temperature;
}

static uint64_t sensor_device_read(void* opaque, hwaddr addr, unsigned int size) {
    SensorDeviceState* s = opaque;
    pthread_mutex_lock(&lock);
    uint64_t value = 0;
    switch (addr) {
    case REG_TEMP: value = s->temperature; break;
    case REG_MIN_TEMP: value = s->min_temp; break;
    case REG_MAX_TEMP: value = s->max_temp; break;
    case REG_CONTROL: value = s->is_sending; break;
    case REG_STATUS: value = s->status; break;
    default: value = 0xDEADBEEF;
    }
    pthread_mutex_unlock(&lock);
    return value;
}

static void sensor_device_write(void* opaque, hwaddr addr, uint64_t value, unsigned int size) {
    SensorDeviceState* s = opaque;
    pthread_mutex_lock(&lock);
    switch (addr) {
    case REG_MIN_TEMP: s->min_temp = (int)value; break;
    case REG_MAX_TEMP: s->max_temp = (int)value; break;
    case REG_STATUS: s->status = (int)value; break;
    case REG_CONTROL:
        if (value && !s->is_sending) {
            s->is_sending = 1;
            pthread_create(&s->send_thread, NULL, send_temperature_thread, s);
            pthread_detach(s->send_thread);
        }
        else if (!value && s->is_sending) {
            s->is_sending = 0;

            if (s->mosq) {
                mosquitto_disconnect(s->mosq);
                mosquitto_destroy(s->mosq);
                s->mosq = NULL;
                printf("❌ Sending stopped.\n");
            }
        }
        break;
    default:

        printf("Unknown write address: 0x%lx, ignoring write\n", addr);
        break;
    }
    pthread_mutex_unlock(&lock);
}

void send_temperature_via_mqtt(SensorDeviceState* s, int temperature) {
    if (!s->is_sending || !s->mosq) return;
    int rc = mosquitto_loop(s->mosq, -1, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Error: MQTT connection lost. Reconnecting...\n");
        while (mosquitto_reconnect(s->mosq) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Retrying MQTT connection...\n");
            sleep(5);
        }
        fprintf(stderr, "Reconnected successfully!\n");
    }
    char payload[128];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long timestamp_ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    snprintf(payload, sizeof(payload), "%ld Temperature: %d°C", timestamp_ms, temperature);

    if (mosquitto_publish(s->mosq, NULL, MQTT_TOPIC, strlen(payload), payload, 1, false) != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Error: Failed to send temperature\n");
    }
    else {
        printf("Sent temperature: %d°C\n", temperature);
    }
}

void* send_temperature_thread(void* arg) {
    SensorDeviceState* s = arg;
    mosquitto_lib_init();
    s->mosq = mosquitto_new("sensor_device", true, NULL);
    if (!s->mosq) {
        fprintf(stderr, "Error: Failed to create MQTT client\n");
        return NULL;
    }
    int attempt = 0;
    int max_attempts = 10;
    int wait_time = 1;
    time_t start_time = time(NULL);

    while (attempt < max_attempts) {
        if (mosquitto_connect(s->mosq, MQTT_BROKER, MQTT_PORT, 60) == MOSQ_ERR_SUCCESS) {
            mosquitto_message_callback_set(s->mosq, on_message);
            mosquitto_user_data_set(s->mosq, s);
            mosquitto_subscribe(s->mosq, NULL, "sensor/ack", 0);
            break;
        }
        else {
            attempt++;
            fprintf(stderr, "Error: Failed to connect to MQTT broker. Attempt %d of %d\n", attempt, max_attempts);
            fprintf(stderr, "Retrying in %d seconds...\n", wait_time);
            sleep(wait_time);
            wait_time = (wait_time < 32) ? wait_time * 2 : 32;
        }

        if (time(NULL) - start_time > 30) {
            fprintf(stderr, "Error: Connection attempts exceeded 30 seconds. Aborting...\n");
            mosquitto_destroy(s->mosq);
            s->mosq = NULL;
            mosquitto_lib_cleanup();
            return NULL;
        }
    }

    if (attempt == max_attempts) {
        fprintf(stderr, "Error: Failed to connect after %d attempts. Aborting.\n", max_attempts);
        mosquitto_destroy(s->mosq);
        s->mosq = NULL;
        mosquitto_lib_cleanup();
        return NULL;
    }

    mosquitto_loop_start(s->mosq);
    pthread_mutex_lock(&lock);
    s->temperature = generate_realistic_temperature(s->temperature);
    pthread_mutex_unlock(&lock);
    send_temperature_via_mqtt(s, s->temperature);

    while (s->is_sending) {
        sleep(2);

        if (s->ack_received_recently) {
            // ACK وصل => تابع الإرسال الطبيعي
            s->ack_received_recently = 0;
            pthread_mutex_lock(&lock);
            s->temperature = generate_realistic_temperature(s->temperature);
            pthread_mutex_unlock(&lock);
            send_temperature_via_mqtt(s, s->temperature);
        }
        else {

            if (!s->ack_received_recently) {
                printf("No ACK received...\n");
                pthread_mutex_lock(&lock);
                s->temperature = generate_realistic_temperature(s->temperature);
                pthread_mutex_unlock(&lock);
                send_temperature_via_mqtt(s, s->temperature);

            }
        }
    }

    mosquitto_loop_stop(s->mosq, false);
    mosquitto_disconnect(s->mosq);
    mosquitto_destroy(s->mosq);
    mosquitto_lib_cleanup();
    return NULL;
}

void on_message(struct mosquitto* mosq, void* userdata, const struct mosquitto_message* message) {
    SensorDeviceState* s = (SensorDeviceState*)userdata;

    if (strcmp(message->topic, "sensor/ack") == 0) {
        char* payload = (char*)message->payload;
        long ack_timestamp;
        float delay = 0.0f;

        if (sscanf(payload, "%ld ACK | Delay: %f ms", &ack_timestamp, &delay) == 2) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            long now_ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000;
            long delay_received = now_ms - ack_timestamp;
            float total_delay = delay_received + delay;
            printf("ACK received | Delay between QEMU and Orange Pi: %ld ms\n", delay_received);
            printf("Total Delay between QEMU and STM32: %.3f ms\n", total_delay);

            s->ack_received_recently = 1;
        }
        else {
            fprintf(stderr, "Invalid ACK format: %s\n", payload);
        }
    }
}


void sensor_device_instance_init(Object* obj) {
    SensorDeviceState* s = SENSOR_DEVICE(obj);
    memory_region_init_io(&s->iomem, obj, &sensor_device_ops, s, TYPE_SENSOR_DEVICE, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->min_temp = 18;
    s->max_temp = 30;
    s->status = 0;
    s->is_sending = 0;
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

type_init(sensor_device_register_types)

void sensor_device_register_types(void) {
    type_register_static(&sensor_device_info);
}

DeviceState* sensor_device_create(hwaddr addr) {
    DeviceState* dev = qdev_new(TYPE_SENSOR_DEVICE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}