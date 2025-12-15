#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <Wire.h>
#include <MQTT.h>
#include <Esp.h>

#define VERSION      "v100"
char header[30] =    "";

#define PIN_LED      15
#define PIN_BUTTON   0
bool screenOn = true;
int buttonState;
unsigned long screensaver;

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
WiFiClient wifiClient;

#define MQTT_SERVER  "192.168.0.86"
#define MQTT_TOPIC   "LoRa"
#define MQTT_CLIENT  "LoRa2MQTT"
#define MQTT_USER    "guest"
#define MQTT_PASSWD  "invitation"
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
#define CHAR_RSSI     0x01

const unsigned char ssd1306_init[] = {
    0xAE | 0x00,          // SET_DISP            off
    0xA0 | 0x01,          // SET_REG_REMAP       horizontal reverse start
    0xA8, LCD_V_RES - 1,  // SET_MUX_RATIO
    0xC0 | 0x08,          // SET_COM_OUT_DIR     horizontal reverse scan
    0xDA, 0x12,           // SET_COM_PIN_CFG     must be 0x02 if aspect ratio > 2:1, 0x12 otherwise
    0x20, 0x02,           // SET_MEM_ADDRESS     page mode
    0xD9, 0xF1,           // SET_PRECHARGE
    0xDB, 0x30,           // SET_VCOM_DESEL
    0x81, 0xFF,           // SET_CONTRAST
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

void displayOn() {
    ssd1306_cmd(0x81);
    ssd1306_cmd(0xFF);
    Serial.println("Screen ON");
    screenOn = true;
    digitalWrite(PIN_LED, !screenOn);
    screensaver = millis();
}

void displayOff() {
    ssd1306_cmd(0x81);
    ssd1306_cmd(0x00);
    Serial.println("Screensaver");
    screenOn = false;
    digitalWrite(PIN_LED, !screenOn);
    buttonState = digitalRead(PIN_BUTTON);
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

    screenOn = true;
    screensaver = millis();
}

unsigned char line_top[LCD_H_RES];
unsigned char line_bot[LCD_H_RES];

void writeLine(int line, char *text) {
    int target = 0;
    int i;

    for (i = 0; i < strlen(text) && i < LINE_WIDTH; i++) {
        int index = text[i];
        for (int col = 0; col < FONT_WIDTH_16; col++) {
            line_top[target] = fontdata_top[index][col];
            line_bot[target] = fontdata_bot[index][col];
            target++;
        }
        line_top[target] = line_bot[target] = 0x00;
        target++;
    }
    for (; i < LINE_WIDTH; i++) {
        for (int col = 0; col <= FONT_WIDTH_16; col++) {
            line_top[target] = line_bot[target] = 0x00;
            target++;
        }
        line_top[target] = line_bot[target] = 0x00;
        target++;
    }

    sendPagePos(line * 2,     0, line_top, target);
    sendPagePos(line * 2 + 1, 0, line_bot, target);
}

void writeHeader(char *header) {
    writeLine(0, header);
}

// --------------------------------
// Global variables shared with ISR

#define MAX_PAYLOAD  256

#define ID_HEADER    0x92
#define SOURCES      3
#define ID_BAL       0
#define ID_CELLAR    1
#define ID_TEST      2
float parseTest(int length, byte *payload, char *message, char *topic, char *json);
float parseBal(int length, byte *payload, char *message, char *topic, char *json);
float parseCellar(int length, byte *payload, char *message, char *topic, char *json);

struct {
    byte code;
    char name[10];
    float (*parser)(int, byte*, char*, char*, char*);
} Vector[SOURCES] = {
    {0xBA, "Bal",    parseBal},
    {0xCE, "Cellar", parseCellar},
    {0xA5, "Test",   parseTest}
};

struct {
    char message[1024];
    char topic[128];
    char json[256];
    unsigned long millis;
    float battery;
    int rssi;
    float snr;
} Rx[SOURCES];
byte payload[MAX_PAYLOAD];

int onScreen = -1;
void sendMessage(int source) {
    char topic[20], info[20];

    onScreen = source;
    sprintf(info, "[%d,%.2f]", Rx[source].rssi, Rx[source].snr);
    sprintf(topic, "%s(0m)", Vector[source].name);
    Serial.printf("%s:%s %s\n", topic, Rx[source].message, info);
    writeLine(1, topic);
    writeLine(2, Rx[source].message);
    writeLine(3, info);

    mqttClient.publish(topic, Rx[source].message);
    if (Rx[source].json[0] != 0) {
        mqttClient.publish(Rx[source].topic, Rx[source].json);
    }
    
    char rssi;
    if (Rx[source].rssi > 135) rssi = 1;
    else if (Rx[source].rssi < 55) rssi = 7;
    else rssi = 7 - (Rx[source].rssi - 55) / 16;

    Rx[source].message[0] = 0;
    mqttClient.loop();
}

void skipMessage(bool reboot) {
    while(LoRa.available() > 0) LoRa.read();
    digitalWrite(PIN_LED, !screenOn);
    if (reboot) ESP.restart();
}

// CONTINUOUS, this is called from ISR!!!
void onReceive(int packetSize) {
    digitalWrite(PIN_LED, screenOn);
    
    if (packetSize == 0 || packetSize > MAX_PAYLOAD) {
        skipMessage(true);
        return;
    }
    
    byte header = LoRa.read();
    if (header != ID_HEADER) {
        skipMessage(true);
        return;
    }
    
    byte senderCode = LoRa.read();
    int length = LoRa.available();
    if (length != packetSize - 2) {
        skipMessage(true);
        return;
    }
    
    int source;
    for (source = 0; source < SOURCES; source++) {
        if (senderCode == Vector[source].code) {
            if (Rx[source].message[0] != 0) {
                skipMessage(true);
                return;
            }

            Rx[source].millis = millis();
            for (int i = 0; i < length; i++) {
                payload[i] = LoRa.read();
            }
            Rx[source].json[0] = 0;
            float battery = Vector[source].parser(length, payload, Rx[source].message, Rx[source].topic, Rx[source].json);
            if (battery == 0.0) {
                skipMessage(true);
                return;
            }
            Rx[source].battery = battery;
            Rx[source].rssi = -LoRa.packetRssi();
            Rx[source].snr  = LoRa.packetSnr();
            break;
        }
    }
    
    if (source == SOURCES) {
        skipMessage(true);
        return;
    }

    skipMessage(false);
}

float parseTest(int length, byte *payload, char *message, char *topic, char *json) {
    memcpy(message, payload, length);
    message[length] = 0;
    return 4.2;
}

float parseBal(int length, byte *payload, char *message, char *topic, char *json) {
    char mail = payload[0] == 1 ? 'Y' : 'N';
    float battery = (float)payload[1] / 10.0;
    sprintf(message, "mail=%c batt=%3.1fV", mail, battery);
    return battery;
}

#define HASS_TOPIC   "homeassistant/sensor/CellLora/state"
#define HASS_MESSAGE "{\"temperature\": %.1f, \"humidity\": %.0f, \"voltage\": %.1f}"

float parseCellar(int length, byte *payload, char *message, char *topic, char *json) {
    float battery = (float)payload[0] / 10.0;
    float temperature = (float)payload[1] + (float)payload[2] / 10.0;
    float humidity = (float)payload[3];

    if (battery > 5.0 || battery < 2.0) return 0.0;
    if (temperature > 50.0 || temperature <= 0.0) return 0.0;
    if (humidity >= 100.0 || humidity <= 10.0) return 0.0;
    
    sprintf(message, "%3.1fV %.1fC %.0f%%", battery, temperature, humidity);
    strcpy(topic, HASS_TOPIC);
    sprintf(json, HASS_MESSAGE, temperature, humidity, battery);
    return battery;
}

// -----------------
// Setup code proper

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.print("Starting ");
    Serial.println(VERSION);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    buttonState = digitalRead(PIN_BUTTON);
    
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
        LoRa.setSpreadingFactor(12);
        LoRa.setSignalBandwidth(31.25E3);
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
    strcat(header, (char*)WiFi.localIP().toString().c_str());
    Serial.println(header);

    mqttClient.begin(MQTT_SERVER, wifiClient);
    mqttClient.setKeepAlive(3600);
    if (!mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASSWD)) {
        Serial.println("Can't connect to MQTT broker");
        delay(1000);
        ESP.restart();
    }
    mqttClient.publish(MQTT_TOPIC, header, true, 0);

    Serial.println("MQTT connected");
    writeHeader(header);

    Serial.println("Listening...");
    LoRa.onReceive(onReceive);
    LoRa.receive();
}

unsigned long millisElapsed(unsigned long end, unsigned long start) {
    if (end >= start) return (end - start);
    unsigned long inverse = start - end - 1;
    unsigned long result = 4294967295L - inverse;
    return result;
}

bool messageSent = false;
void loop() {
   static unsigned long lastUpdate_ms = millis();
    
    for (int source = 0; source < SOURCES; source ++) {
        if (Rx[source].message[0] != 0) {
            sendMessage(source);
            messageSent = true;
        }
    }

    if (onScreen >= 0) {
        unsigned long seconds;
        char elapsedTime[8];
        if (millisElapsed(millis(), lastUpdate_ms) > 1000L * 60) {
            seconds = millisElapsed(millis(), Rx[onScreen].millis) / 1000;
            if (seconds >= 3600 * 24 * 99) {
                strcpy(elapsedTime, "Inf");
            } else if (seconds >= 3600 * 24) {
                sprintf(elapsedTime, "%dd", seconds / 3600 / 24);
            } else if (seconds >= 3600) {
                sprintf(elapsedTime, "%dh", seconds / 3600);
            } else {
                sprintf(elapsedTime, "%dm", seconds / 60);
            }
            
            char topic[20];
            sprintf(topic, "%s(%s)", Vector[onScreen].name, elapsedTime);
            writeLine(1, topic);
        
            lastUpdate_ms = millis();
        }
    }
    
    if (screenOn) {
        if (millisElapsed(millis(), screensaver) > 1000L * 30) {
            displayOff();
        }
    } else {
        if (buttonState != digitalRead(PIN_BUTTON)) {
            displayOn();
        }
    }

    if (!mqttClient.connected()) {
      Serial.println("MQTT failure");
      delay(1000);
      ESP.restart();
    }
    mqttClient.loop();

    sleep(1);
    if (messageSent) ESP.restart();
}
