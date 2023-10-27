#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <Wire.h>
#include <MQTT.h>
#include <Esp.h>

char VERSION_STR[30] = "v100 ";

#define PIN_LED      15

#define LORA_RST     39
#define LORA_SCK     37
#define LORA_MISO    35
#define LORA_SS      33
#define LORA_MOSI    18
#define LORA_INT     16

#define I2C_SCL      3
#define I2C_SDA      5

#define SSID         "HORS SERVICE"
#define PASS         "babeface00"
WiFiClient wifiClient(2048);

#define MQTT_SERVER  "jayhan.name"
#define MQTT_TOPIC   "LoRa"
#define MQTT_CLIENT  "LoRa2MQTT"
MQTTClient mqttClient(1024);

// ----------------------------
// I2C for OLED display SSD1306

#define I2C_ADDR     0x3C

#define FONT_WIDTH    FONT_WIDTH_16
#define FONT_PITCH    (FONT_WIDTH + 1)
#define LCD_H_RES     128
#define LCD_V_RES     64
#define LCD_PAGES     (LCD_V_RES / 8)
#define LINE_WIDTH    (LCD_H_RES / FONT_PITCH)
#include "fontdata.h"

const unsigned char ssd1306_init[] = {
    0xAE | 0x00,          // SET_DISP            off
    0xA0 | 0x01,          // SET_REG_REMAP       horizontal reverse start
    0xA8, LCD_V_RES - 1,  // SET_MUX_RATIO
    0xC0 | 0x08,          // SET_COM_OUT_DIR     horizontal reverse scan
    0xDA, 0x12,           // SET_COM_PIN_CFG     must be 0x02 if aspect ratio > 2:1, 0x12 otherwise
    0x20, 0x02,           // SET_MEM_ADDRESS     page mode
    0xD9, 0xF1,           // SET_PRECHARGE
    0xDB, 0x30,           // SET_VCOM_DESEL
    0x81, 0x7F,           // SET_CONTRAST
    0xA4,                 // SET_ENTIRE_ON
    0xA6 | 0x00,          // SET_NORM_INV (0x01 for inverse)
    0x8D, 0x14,           // SET_CHARGE_PUMP
    0xAE | 0x01,          // SET_DISP            on
};
unsigned char init_buffer[LCD_H_RES / 2 + 1];

void ssd1306_cmd(unsigned char cmd) {
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(0x80);
    Wire.write(cmd);
    Wire.endTransmission();
}

void sendPagePos(int page, int pos, unsigned char *thisBuffer, int length) {
    ssd1306_cmd(0xB0 | page);
    ssd1306_cmd(0x00 | (pos & 0x0F));
    ssd1306_cmd(0x10 | (pos >> 4));
        
    Wire.beginTransmission(I2C_ADDR);
    Wire.write(0x40);
    Wire.write(thisBuffer, length);
    Wire.endTransmission();
}

void initDisplay() {
    memset(init_buffer, 0, sizeof(init_buffer));
    Wire.begin(I2C_SDA, I2C_SCL, 400000);
    Wire.beginTransmission(I2C_ADDR);
    if (Wire.endTransmission() == 0) {
        for (int i = 0; i < sizeof(ssd1306_init); i++) {
            ssd1306_cmd(ssd1306_init[i]);
        }

        for (int page = 0; page < LCD_PAGES; page ++) {
            sendPagePos(page, 0, init_buffer, sizeof(init_buffer));
            sendPagePos(page, LCD_H_RES / 2, init_buffer, sizeof(init_buffer));
        }
        Serial.println("I2C OLED initialized");
    } else {
        Serial.println("I2C OLED failed");
    }
}

unsigned char line_top[LCD_H_RES];
unsigned char line_bot[LCD_H_RES];
void writeDisplay(int line, int pos, char *message) {
    int target = 0;
    int start  = pos * FONT_PITCH;
    for (int i = 0; i < strlen(message) && i < LINE_WIDTH - pos; i++) {
        int index = message[i];
        for (int col = 0; col < FONT_WIDTH_16; col++) {
            line_top[start + target] = fontdata_top[index][col];
            line_bot[start + target] = fontdata_bot[index][col];
            target++;
        }
        line_top[start + target] = line_bot[start + target] = 0x00;
        target++;
    }

    sendPagePos(line * 2,     start, line_top + start, target);
    sendPagePos(line * 2 + 1, start, line_bot + start, target);
}

// --------------------------------
// Global variables shared with ISR

#define CONTINUOUS   1
#define MAX_PAYLOAD  256

#define ID_HEADER    0x92
#define SOURCES      3
#define ID_BAL       0
#define ID_CELLAR    1
#define ID_TEST      2
void parseTest(int length, byte *payload, char *message);
void parseBal(int length, byte *payload, char *message);
void parseCellar(int length, byte *payload, char *message);

struct {
    byte code;
    int line;
    char name[10];
    void (*parser)(int, byte*, char*);
} Vector[SOURCES] = {
    {0xBA, 1, "Bal", parseBal},
    {0xCE, 2, "Cellar", parseCellar},
    {0xA5, 3, "Test", parseTest}
};

struct {
    char message[1024];
    unsigned long millis;
    int rssi;
    float snr;
} Rx[SOURCES];
byte payload[MAX_PAYLOAD];

void sendMessage(int source) {
    char line[LINE_WIDTH + 1], topic[20], info[20], name[4];

    sprintf(info, "[%03d,%.2f]", Rx[source].rssi, Rx[source].snr);
    strcat(Rx[source].message, info);
    sprintf(topic, "%s/%s", MQTT_TOPIC, Vector[source].name);
    Serial.printf("%s:%s\n", topic, Rx[source].message);
    mqttClient.publish(topic, Rx[source].message);
    strncpy(name, Vector[source].name, 3);
    name[3] = 0;
    sprintf(line, "%s  0m %3d %2.0f", name, Rx[source].rssi, Rx[source].snr);
    writeDisplay(Vector[source].line, 0, line);

    Rx[source].message[0] = 0;
}

void skipMessage() {
    while(LoRa.available() > 0) LoRa.read();
    digitalWrite(PIN_LED, LOW);
}

// If CONTINUOUS, this is called from ISR!!!
void onReceive(int packetSize) {
    digitalWrite(PIN_LED, HIGH);
    
    #if !CONTINUOUS
    Serial.printf("Received packet of %d bytes\n", packetSize);
    #endif
    if (packetSize == 0 || packetSize > MAX_PAYLOAD) {
        skipMessage();
        return;
    }
    
    byte header = LoRa.read();
    if (header != ID_HEADER) {
#if !CONTINUOUS
        Serial.printf("Wrong header byte 0x%02X, skipping\n", header);
#endif
        skipMessage();
        return;
    }
    
    byte senderCode = LoRa.read();
    int length = LoRa.available();
    if (length != packetSize - 2) {
#if !CONTINUOUS
        Serial.printf("Packet size mismatch %d != %d\n", length, packetSize - 2);
#endif
        skipMessage();
        return;
    }
    
#if !CONTINUOUS
    Serial.printf("Packet from 0x%02X, data %d bytes\n", senderCode, length);
#endif

    int source;
    for (source = 0; source < SOURCES; source++) {
        if (senderCode == Vector[source].code) {
            if (Rx[source].message[0] != 0) {
                #if !CONTINUOUS
                Serial.println("Overlapping packet, skipping");
                #endif
                skipMessage();
                return;
            }

            Rx[source].millis = millis();
            for (int i = 0; i < length; i++) {
                payload[i] = LoRa.read();
            }
            Vector[source].parser(length, payload, Rx[source].message);
            Rx[source].rssi = -LoRa.packetRssi();
            Rx[source].snr  = LoRa.packetSnr();
#if !CONTINUOUS
            sendMessage(source);
#endif
            break;
        }
    }
    
    if (source == SOURCES) {
#if !CONTINUOUS
        Serial.printf("Unknown source 0x%02X, skipping\n", senderCode);
#endif
        skipMessage();
        return;
    }

    skipMessage();
}

void parseTest(int length, byte *payload, char *message) {
    memcpy(message, payload, length);
    message[length] = 0;
}

void parseBal(int length, byte *payload, char *message) {
    char mail = payload[0] == 1 ? 'Y' : 'N';
    float batt = (float)payload[1] + (float)payload[2] * 0.01;
    sprintf(message, "mail=%c batt=%.2fV", mail, batt);
}

void parseCellar(int length, byte *payload, char *message) {
    int numData = length / 2;
    byte *data = payload;
    char *string = message;
    for (int item = 0; item < numData; item++) {
        int temperature = data[0];
        if (temperature >= 128) temperature -= 256;
        if (temperature > 99) temperature = 99;
        int humidity = data[1];
        if (humidity > 99) humidity = 99;
        sprintf(string, "(%d,%d)", temperature, humidity);
        
        string += strlen(string);
        data += 2;
    }
}

// -----------------
// Setup code proper

void setup() {
    Serial.begin(921600);
    delay(1000);
    Serial.print("Starting ");
    Serial.println(VERSION_STR);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, HIGH);
    WiFi.begin(SSID, PASS);

    initDisplay();
    
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
    strcat(VERSION_STR, (char*)WiFi.localIP().toString().c_str());
    Serial.println(VERSION_STR);

    mqttClient.begin(MQTT_SERVER, wifiClient);
    mqttClient.setKeepAlive(3600);
    if (!mqttClient.connect(MQTT_CLIENT)) {
        Serial.println("Can't connect to MQTT broker");
        delay(1000);
        ESP.restart();
    }
    mqttClient.publish(MQTT_TOPIC, VERSION_STR, true, 0);
    Serial.println("MQTT connected");
    writeDisplay(0, 0, VERSION_STR);
    digitalWrite(PIN_LED, LOW);

#if CONTINUOUS      
    LoRa.onReceive(onReceive);
    LoRa.receive();
#endif
}

unsigned long millisElapsed(unsigned long end, unsigned long start) {
    if (end >= start) return (end - start);
    unsigned long inverse = start - end - 1;
    unsigned long result = 4294967295L - inverse;
    return result;
}

void loop() {
    static unsigned long lastUpdate_ms = millis();
    
#if CONTINUOUS
    for (int source = 0; source < SOURCES; source ++) {
        if (Rx[source].message[0] != 0) {
            sendMessage(source);
        }
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

    char timeElapsed[4];
    unsigned long seconds;
    if (millisElapsed(millis(), lastUpdate_ms) > 1000L * 60) {
        for (int source = 0; source < SOURCES; source ++) {
            seconds = millisElapsed(millis(), Rx[source].millis) / 1000;
            if (seconds >= 3600 * 100) {
                strcpy(timeElapsed, "Inf");
            } else if (seconds >= 3600) {
                sprintf(timeElapsed, "%2dh", seconds / 3600);
            } else if (seconds >= 60) {
                sprintf(timeElapsed, "%2dm", seconds / 60);
            } else {
                continue;
            }
            writeDisplay(Vector[source].line, 4, timeElapsed);
        }
        lastUpdate_ms = millis();
    }
        
    mqttClient.loop();
}

