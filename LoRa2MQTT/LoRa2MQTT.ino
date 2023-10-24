#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <MQTT.h>
#include <Esp.h>

#define PIN_LED      15

#define LORA_SS      2
#define LORA_RST     4
#define LORA_SCK     6
#define LORA_MOSI    8
#define LORA_MISO    10
#define LORA_INT     13

#define CONTINUOUS   0
#define MAX_PAYLOAD  256
#define INFO_SIZE    20

#include "driver/uart.h"
#define UART_RX      40
#define UART_TX      38

#define ID_HEADER    0x92
#define SOURCES      3
char *parseTest(int length, byte *payload, int rssi, float snr);
char *parseBal(int length, byte *payload, int rssi, float snr);
char *parseCellar(int length, byte *payload, int rssi, float snr);
struct {
    byte address;
    char topic[10];
    char *(*parser)(int, byte*, int, float);
} Vector[SOURCES] = {
    {0xA5, "Test", parseTest},
    {0xBA, "Bal", parseBal},
    {0xCE, "Cellar", parseCellar}
};

#define SSID         "HORS SERVICE"
#define PASS         "babeface00"
WiFiClient wifiClient;

#define MQTT_SERVER  "jayhan.name"
#define MQTT_PORT    1883
#define MQTT_TOPIC   "LoRa"
#define MQTT_CLIENT  "LoRa2MQTT"
MQTTClient mqttClient;

int blink = 0;
unsigned int blinkTime;
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, 1);
    WiFi.begin(SSID, PASS);
    
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    pinMode(LORA_INT, INPUT);
    LoRa.setPins(LORA_SS, LORA_RST, LORA_INT);

    if (!LoRa.begin(433E6)) {
        Serial.println("LoRa begin fail");
        delay(1000);
        ESP.restart();
    } else {
        LoRa.setSpreadingFactor(10);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(8);
        Serial.println("LoRa initialized");
        digitalWrite(PIN_LED, 1);
    }

    int wait = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        digitalWrite(PIN_LED, wait % 2);
        wait ++;
        if (wait > 20) {
            Serial.println("WiFi failed");
            delay(1000);
            ESP.restart();
        }
    }
    digitalWrite(PIN_LED, 1);
    Serial.println(WiFi.localIP());

    mqttClient.begin(MQTT_SERVER, wifiClient);
    mqttClient.setKeepAlive(3600);
    if (!mqttClient.connect(MQTT_CLIENT)) {
        Serial.println("Can't connect to MQTT broker");
        delay(1000);
        ESP.restart();
    }
    mqttClient.publish(MQTT_TOPIC, WiFi.localIP().toString().c_str(), true, 0);
    Serial.println("MQTT connected");
    digitalWrite(PIN_LED, 0);

#if CONTINUOUS      
    LoRa.onReceive(onReceive);
    LoRa.receive();
#else
    blinkTime = millis() + 500;
#endif
}

void loop() {
#if CONTINUOUS
    delay(1000);
    blink = 1 - blink;
    digitalWrite(PIN_LED, blink);
#else
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) {
        if (millis() >= blinkTime) {
            blink = 1 - blink;
            digitalWrite(PIN_LED, blink);
            blinkTime = millis() + 500;
            Serial.print(".");
        }
    } else {
        Serial.println("INT");
        blink = 1;
        digitalWrite(PIN_LED, blink);
        blinkTime = millis() + 2000;
        onReceive(packetSize);
    }
#endif    
}

char topic[20], info[INFO_SIZE];
byte payload[MAX_PAYLOAD];
void onReceive(int packetSize) {
    Serial.printf("Received packed %d bytes\n", packetSize);
    if (packetSize == 0) return;
    if (packetSize > MAX_PAYLOAD) {
        Serial.println("Packet too big");
        return;
    }
    digitalWrite(PIN_LED, 1);
    byte header = LoRa.read();
    if (header != ID_HEADER) {
        Serial.printf("Wrong header byte 0x%02X, skipping\n", header);
    } else {
        byte whoisit = LoRa.read();
        int source;
        for (source = 0; source < SOURCES; source++) {
            if (whoisit == Vector[source].address) {
                sprintf(topic, "%s/%s", MQTT_TOPIC, Vector[source].topic);
                int length = LoRa.readBytes(payload, packetSize - 2);
                char *message = Vector[source].parser(length, payload,
                                                      LoRa.packetRssi(), LoRa.packetSnr());
                if (message != NULL) {
                    Serial.printf("%s:%s\n", topic, message);
                    mqttClient.publish(topic, message);
                    free(message);
                } else {
                    Serial.printf("Couldn't parse %s message length %d\n",
                                  Vector[source].topic, length);
                }
                break;
            }
        }
        if (source == SOURCES) {
            Serial.printf("Unknown source 0x%02X, skipping\n", whoisit);
        }
    }
    digitalWrite(PIN_LED, 0);
}

char *parseTest(int length, byte *payload, int rssi, float snr) {
    char *message = (char*)malloc(length + INFO_SIZE);
    memcpy(message, payload, length);
    sprintf(message + length, "[%d,%.2f]", rssi, snr);
    return message;
}

char *parseBal(int length, byte *payload, int rssi, float snr) {
    char *message = (char*)malloc(20 + INFO_SIZE);
    char mail = payload[0] == 1 ? 'Y' : 'N';
    float batt = (float)payload[1] + (float)payload[2] * 0.01;
    sprintf(message, "mail=%c batt=%.2fV", mail, batt);
    sprintf(message + strlen(message), "[%d,%.2f]", rssi, snr);
    return message;
}

char *parseCellar(int length, byte *payload, int rssi, float snr) {
    int dataSize = length / 2;
    char *message = (char*)malloc(7 * dataSize + 1 + INFO_SIZE);
    
    byte *data = payload;
    char *string = message;
    for (int item = 0; item < dataSize; item++) {
        int temperature = data[0];
        if (temperature > 99) temperature = 99;
        int humidity = data[1];
        if (humidity > 99) humidity = 99;
        sprintf(string, "(%02d,%02d)", temperature, humidity);
        
        string += strlen(string);
        data += 2;
    }
    sprintf(string, "[%d,%.2f]", rssi, snr);
    return message;
}
