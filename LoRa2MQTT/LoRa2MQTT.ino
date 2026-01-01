#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <Wire.h>
#include <MQTT.h>
#include <Esp.h>

#define VERSION      "v200"

#define PIN_LED      15
#define PIN_BUTTON   0

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
#define LINE_WIDTH    (LCD_H_RES / FONT_PITCH / 2)
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

byte nybble_low(byte original) {
    byte result = 0;
    for (int pos = 0; pos < 4; pos++) {
        if (original & (1 << pos)) result |= (3 << (pos * 2));
    }
    return result;
}

byte nybble_hi(byte original) {
    byte result = 0;
    for (int pos = 0; pos < 4; pos++) {
        if (original & (1 << (4 + pos))) result |= (3 << (pos * 2));
    }
    return result;
}

unsigned char lines[4][LCD_H_RES];

void writeBig(int line, char *text) {
    int target = 0;
    int i;

    for (i = 0; i < strlen(text) && i < LINE_WIDTH; i++) {
        int index = text[i];
        for (int col = 0; col < FONT_WIDTH_16; col++) {
            lines[0][target] = nybble_low(fontdata_top[index][col]);
            lines[1][target] = nybble_hi(fontdata_top[index][col]);
            lines[2][target] = nybble_low(fontdata_bot[index][col]);
            lines[3][target] = nybble_hi(fontdata_bot[index][col]);
            target++;
            lines[0][target] = nybble_low(fontdata_top[index][col]);
            lines[1][target] = nybble_hi(fontdata_top[index][col]);
            lines[2][target] = nybble_low(fontdata_bot[index][col]);
            lines[3][target] = nybble_hi(fontdata_bot[index][col]);
            target++;
        }
        lines[0][target] = lines[1][target] = lines[2][target] = lines[3][target] = 0x00;
        target++;
        lines[0][target] = lines[1][target] = lines[2][target] = lines[3][target] = 0x00;
        target++;
    }
    
    for (; target < LCD_H_RES; target++) {
        lines[0][target] = lines[1][target] = lines[2][target] = lines[3][target] = 0x00;
    }

    for (int sub = 0; sub < 4; sub++) {
        sendPagePos(line * 4 + sub, 0, lines[sub], target);
    }
}

// --------------------------------
// Global variables shared with ISR

#define MAX_PAYLOAD  16

#define ID_CELLAR1   0xCE
#define ID_CELLAR2   0xCF

struct {
    char message[16];
    char json[128];
    float battery;
    int rssi, snr;
} Rx;
byte payload[MAX_PAYLOAD];

#define HASS_TOPIC  "homeassistant/sensor/CellLora/state"
#define HASS_JSON   "{\"temperature\": %.1f, \"humidity\": %.0f, \"voltage\": %.1f}"

void sendMessage() {
    mqttClient.publish(MQTT_TOPIC, Rx.message);
    mqttClient.loop();
    sleep(1);
    mqttClient.publish(HASS_TOPIC, Rx.json);
    mqttClient.loop();
    sleep(1);
}

char messageText[8];
char dataText[8];
void skipMessage(char *text) {
    while(LoRa.peek() >= 0) LoRa.read();
    if (text != NULL) strcpy(messageText, text);
    messageText[5] = 0;
}

// CONTINUOUS, this is called from ISR!!!
bool isReceived = false;
void onReceive(int packetSize) {
    if (isReceived) {
        skipMessage("DUP");
        return;
    }
    
    if (packetSize == 0 || packetSize > MAX_PAYLOAD) {
        skipMessage("SIZE");
        return;
    }
    
    byte senderCode = LoRa.read();
    if (senderCode != ID_CELLAR1 && senderCode != ID_CELLAR2) {
        skipMessage("CODE");
        return;
    }
    
    int length = LoRa.available();
    if (length != packetSize - 1) {
        skipMessage("PAYL");
        return;
    }
    
    for (int i = 0; i < length; i++) {
        payload[i] = LoRa.read();
    }
    
    float battery = (float)payload[0] / 10.0;
    float temperature = (float)payload[1] + (float)payload[2] / 10.0;
    float humidity = (float)payload[3];

    isReceived = false;
    do {
        if (battery > 5.0 || battery < 2.0) break;
        if (temperature > 50.0 || temperature <= 0.0) break;
        if (humidity >= 100.0 || humidity <= 10.0) break;
    
        sprintf(Rx.message, "%.1f %.0f", battery, temperature, humidity);
        sprintf(Rx.json, HASS_JSON, temperature, humidity, battery);
        
        Rx.battery = battery;
        Rx.rssi = -LoRa.packetRssi();
        Rx.snr  = LoRa.packetSnr() * 10.0;

        sprintf(dataText, "%3d %3d", Rx.rssi, Rx.snr);
        skipMessage("");
        isReceived = true;
    } while(false);
    
    if (!isReceived) {
        skipMessage("DATA");
    }
}

// -----------------
// Setup code proper

void setup() {
    Serial.begin(115200);
    
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, HIGH);
    delay(200);
    digitalWrite(LORA_RST, LOW);
    delay(200);
    digitalWrite(LORA_RST, HIGH);
    delay(200);
    
    delay(1000);
    Serial.print("Starting ");
    Serial.println(VERSION);

    pinMode(PIN_BUTTON, INPUT_PULLUP);
    
    WiFi.begin(SSID, PASS);

    initDisplay();
    writeBig(0, "START");

    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    pinMode(LORA_INT, INPUT_PULLUP);
    LoRa.setPins(LORA_SS, LORA_RST, LORA_INT);

    if (!LoRa.begin(433E6)) {
        Serial.println("LoRa begin fail");
        sleep(1);
        delay(1000);
    } else {
        LoRa.setSpreadingFactor(12);
        LoRa.setCodingRate4(8);
        LoRa.enableCrc();
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
    
    char ip[30] = "";
    strcat(ip, (char*)WiFi.localIP().toString().c_str());
    Serial.println(ip);

    mqttClient.begin(MQTT_SERVER, wifiClient);
    mqttClient.setKeepAlive(3600);
    if (!mqttClient.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASSWD)) {
        Serial.println("Can't connect to MQTT broker");
        delay(1000);
        ESP.restart();
    }
    mqttClient.publish(MQTT_TOPIC, ip, true, 0);

    Serial.println("MQTT connected");

    Serial.println("Listening...");
    writeBig(0, "OK");
    
    LoRa.onReceive(onReceive);
    LoRa.receive();
}

bool messageSent = false;
void loop() {
    static int lastReceived = 0;
    static int lastUpdate = -1;

    if (isReceived) {
       sendMessage();
       lastReceived = millis() / 60000;
       isReceived = false;
    }
    if (dataText[0] != 0) {
        writeBig(1, dataText);
        Serial.println(dataText);
        dataText[0] = 0;
    }

    int minutes = millis() / 60000 - lastReceived;
    if (minutes != lastUpdate || messageText[0] != 0) {
        char timeText[8];
        lastUpdate = minutes;
        
        sprintf(timeText, "%2d %s", minutes, messageText);
        messageText[0] = 0;
        writeBig(0, timeText);
        
        if (minutes > 60) {
            Serial.println("No data");
            sleep(1);
            ESP.restart();
        }
    }
    
    if (!mqttClient.connected()) {
      Serial.println("MQTT failure");
      sleep(1);
      ESP.restart();
    }
    mqttClient.loop();
}
