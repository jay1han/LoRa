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

#define CONTINUOUS   1
#define MAX_PAYLOAD  256
#define INFO_SIZE    20

#include "driver/uart.h"
#define UART_RX      40
#define UART_TX      38

#define ID_HEADER    0x92
#define SOURCES      3
void parseTest(int length, byte *payload, int rssi, float snr);
void parseBal(int length, byte *payload, int rssi, float snr);
void parseCellar(int length, byte *payload, int rssi, float snr);
struct {
    byte address;
    char topic[10];
    void (*parser)(int, byte*, int, float);
} Vector[SOURCES] = {
    {0xA5, "Test", parseTest},
    {0xBA, "Bal", parseBal},
    {0xCE, "Cellar", parseCellar}
};

#define SSID         "HORS SERVICE"
#define PASS         "babeface00"
WiFiClient wifiClient(2048);

#define MQTT_SERVER  "jayhan.name"
#define MQTT_PORT    1883
#define MQTT_TOPIC   "LoRa"
#define MQTT_CLIENT  "LoRa2MQTT"
MQTTClient mqttClient(1024);

char topic[20], info[INFO_SIZE], message[1024] = {0};
byte payload[MAX_PAYLOAD];
// If CONTINUOUS, this is called from ISR!!!
void onReceive(int packetSize) {
#if !CONTINUOUS
    Serial.printf("Received packet of %d bytes\n", packetSize);
#endif
    if (packetSize == 0 || packetSize > MAX_PAYLOAD) return;

    byte header = LoRa.read();
    digitalWrite(PIN_LED, HIGH);
    if (header != ID_HEADER) {
#if !CONTINUOUS
        Serial.printf("Wrong header byte 0x%02X, skipping\n", header);
#endif        
    } else {
        byte whoisit = LoRa.read();
        int length = LoRa.available();
#if !CONTINUOUS
        Serial.printf("Packet from 0x%02X, remaining %d bytes\n", whoisit, length);
#endif        
        
        int source;
        for (source = 0; source < SOURCES; source++) {
            if (whoisit == Vector[source].address) {
                sprintf(topic, "%s/%s", MQTT_TOPIC, Vector[source].topic);
                for (int i = 0; i < packetSize - 2; i++) {
                    payload[i] = LoRa.read();
                }
                message[0] = 0;
                Vector[source].parser(packetSize - 2, payload,
                                      LoRa.packetRssi(), LoRa.packetSnr());
#if !CONTINUOUS
                Serial.printf("%s:%s\n", topic, message);
                mqttClient.publish(topic, message);
#endif
                break;
            }
        }
        if (source == SOURCES) {
#if !CONTINUOUS
            Serial.printf("Unknown source 0x%02X, skipping\n", whoisit);
#endif            
        }
    }
    digitalWrite(PIN_LED, LOW);
}

void parseTest(int length, byte *payload, int rssi, float snr) {
    memcpy(message, payload, length);
    sprintf(message + length, "[%d,%.2f]", rssi, snr);
}

void parseBal(int length, byte *payload, int rssi, float snr) {
    char mail = payload[0] == 1 ? 'Y' : 'N';
    float batt = (float)payload[1] + (float)payload[2] * 0.01;
    sprintf(message, "mail=%c batt=%.2fV", mail, batt);
    sprintf(message + strlen(message), "[%d,%.2f]", rssi, snr);
}

void parseCellar(int length, byte *payload, int rssi, float snr) {
    int dataSize = length / 2;
    byte *data = payload;
    char *string = message;
    for (int item = 0; item < dataSize; item++) {
        int temperature = data[0];
        if (temperature >= 128) temperature -= 256;
        if (temperature > 99) temperature = 99;
        int humidity = data[1];
        if (humidity > 99) humidity = 99;
        sprintf(string, "(%+03d,%02d)", temperature, humidity);
        
        string += strlen(string);
        data += 2;
    }
    sprintf(string, "[%d,%.2f]", rssi, snr);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    WiFi.begin(SSID, PASS);
    
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    pinMode(LORA_INT, INPUT_PULLUP);
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
    }

    int wait = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        wait ++;
        if (wait > 20) {
            Serial.println("WiFi failed");
            delay(1000);
            ESP.restart();
        }
    }
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

#if CONTINUOUS      
    LoRa.onReceive(onReceive);
    LoRa.receive();
#endif
}

void loop() {
#if CONTINUOUS
    if (message[0] != 0) {
        Serial.printf("%s:%s\n", topic, message);
        mqttClient.publish(topic, message);
        message[0] = 0;
    }
#else
    int packetSize = LoRa.parsePacket();
    if (packetSize == 0) {
        Serial.print(".");
    } else {
        Serial.println("INT");
        onReceive(packetSize);
    }
#endif    
    mqttClient.loop();
}

