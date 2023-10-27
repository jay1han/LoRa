#include "esp_sleep.h"
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Esp.h>
#include "FFat.h"
#include "driver/uart.h"

char VERSION[30] = "v000 ";

#define PIN_LED    8
#define UART_TX    21
#define UART_RX    20
#define PIN_ADC    0
#define I2C_SDA    1
#define I2C_SCL    2
#define I2C_AHT10  0x38

#define LORA_POWER 4
#define LORA_MOSI  5
#define LORA_SS    6
#define LORA_MISO  7
#define LORA_SCK   9
#define LORA_RST   10
#define LORA_INT   -1

#define HISTORY_CSV "/history.csv"
#define HISTORY_OLD "/history.old"
#define HISTORY_NEW "/history.new"

#define SLEEP_TIME_FULL  (3600 * 1000000)
#define SLEEP_TIME_RETRY (600 * 1000000)
uint64_t sleepTime;
#define MAX_HISTORY    (3 * 24)
typedef struct {
    time_t time;
    float temperature;
    float humidity;
} HistoryItem;

// Everything happens in setup()

unsigned long processTime;
void setup() {
    processTime = millis();
    sleepTime = SLEEP_TIME_RETRY; // In case something goes wrong
    uart_set_pin(UART_NUM_0, UART_TX, UART_RX, -1, -1);
    Serial.begin(115200);

// Initialize LoRa module    
    pinMode(LORA_POWER, OUTPUT);
    digitalWrite(LORA_POWER, HIGH);
    pinMode(PIN_ADC, ANALOG);
    adcAttachPin(PIN_ADC);
    pinMode(PIN_LED, INPUT);
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    LoRa.setPins(LORA_SS, LORA_RST);
    if (LoRa.begin(433E6) == 0) {
        Serial.println("LoRa begin fail");
        return;
    } else {
        LoRa.setSignalBandwidth(7.8E3);
        LoRa.setSpreadingFactor(12);
        LoRa.enableCrc();
    }

// Initialize AHT10
    Serial.print("Initialize AHT10 ");
    Wire.begin(I2C_SDA, I2C_SCL, 400000);
    Wire.beginTransmission(I2C_AHT10);
    Wire.write(0xE1);
    Wire.write(0x08);
    Wire.write(0x00);
    if (Wire.endTransmission() == 0) {
        Serial.print("...");
        if (Wire.requestFrom(I2C_AHT10, 1) == 1) {
            byte status = Wire.read();
            Serial.printf("AHT10 Status=0x%02X ", status);
            if ((status & 0x68) == 0x08) {
                Serial.println("OK");
            } else {
                Serial.println("NG");
                return;
            }
        } else {
            Serial.println("not responding");
            return;
        }
    } else {
        Serial.println("not found");
        return;
    }

// Read AHT10 data
    float temperature = -50.0, humidity = 0.0;
    int length = 0;
    byte buffer[6];
    
    Serial.print("AHT10 read data... ");
    unsigned long aht10Time = millis();
    Wire.beginTransmission(I2C_AHT10);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
        Serial.println("send command failed");
        return;
    } else {
        delay(100);

        while (true) {
            Serial.print(".");
            length = Wire.requestFrom(I2C_AHT10, 6);
            if (length < 6) {
                delay(100);
            } else {
                length = Wire.readBytes(buffer, 6);
                if (length == 6 && (buffer[0] & 0x80) == 0) break;
                else delay(100);
            }
        }
        Serial.printf(" Done in %dms\n", millis() - aht10Time);

        unsigned int rawHum  = ((buffer[1] << 16) | (buffer[2] << 8) | buffer[3]) >> 4;
        unsigned int rawTemp = ((buffer[3] & 0x0F) << 16) | (buffer[4] << 8) | buffer[5];
        humidity    = (100.0 * (float)rawHum) / (1 << 20);
        temperature = ((200.0 * (float)rawTemp) / (1 << 20)) - 50.0;
        Serial.printf("Temp=%d=%.2f Hum=%d=%.2f\n",
                      rawTemp, temperature,
                      rawHum,  humidity);
    }

// Read battery voltage    
    int mV = 0;
    for (int i = 0; i < 11; i++) {
        mV += analogReadMilliVolts(PIN_ADC);
        delay(10);
    }
    float Voltage = mV / 1000.0;
    Serial.printf("Battery %.2fV\n", Voltage);

// Build history data item
    if (!FFat.begin(true)) {
        Serial.println("FatFs problem");
        return;
    }

    HistoryItem thisItem;
    thisItem.time        = time(NULL);
    thisItem.temperature = temperature;
    thisItem.humidity    = humidity;
    
// Write history file
    File historyFile = FFat.open(HISTORY_CSV, FILE_READ);
    File updateFile  = FFat.open(HISTORY_NEW, FILE_WRITE);

    if (historyFile && updateFile) {
        updateFile.write((byte*)&thisItem, sizeof(HistoryItem));
        int historySize = 1;
        HistoryItem backupItem;
        while(historyFile.available() && historySize < MAX_HISTORY) {
            historyFile.read((byte*)&backupItem, sizeof(HistoryItem));
            updateFile.write((byte*)&backupItem, sizeof(HistoryItem));
            historySize ++;
        }
        historyFile.close();
        updateFile.close();

        FFat.rename(HISTORY_CSV, HISTORY_OLD);
        FFat.rename(HISTORY_NEW, HISTORY_CSV);
        FFat.remove(HISTORY_OLD);
    } else {
        Serial.println("Can't open files");
        return;
    }

// Send LoRa packet
    if (LoRa.beginPacket() == 0) {
        Serial.println("\nLoRa can't start packet");
        return;
    } else {
        File historyFile = FFat.open(HISTORY_CSV, FILE_READ);
        HistoryItem historyItem;
        byte temperature, humidity;
        unsigned long time;
        int itemCount = 0;

        Serial.print("Sending history data");
        while(historyFile.available()) {
            historyFile.read((byte*)&historyItem, sizeof(HistoryItem));
            if (int(historyItem.temperature) < 0)
                temperature = 255 - int(historyItem.temperature);
            else
                temperature = int(historyItem.temperature);
            humidity = int(historyItem.humidity);
            time = historyItem.time;
            LoRa.write(time >> 24);
            LoRa.write((time >> 16) & 0xFF);
            LoRa.write((time >> 8) & 0xFF);
            LoRa.write(time & 0xFF);
            LoRa.write(temperature);
            LoRa.write(humidity);
            itemCount ++;

            // Start a new packet after 24 hours of history
            if (itemCount == 24) {
                if (LoRa.endPacket() == 0) {
                    Serial.println(" can't send packet");
                    return;
                } else {
                    if (LoRa.beginPacket() == 0) {
                        Serial.println("\nCan't start more packets");
                        return;
                    } else {
                        Serial.print(".");
                    }
                    itemCount = 0;
                }
            }
        }
        historyFile.close();
        
        // All done, send final packet
        if (LoRa.beginPacket() == 0) {
            Serial.println(" can't finish packet");
            return;
        } else {
            Serial.println("Sent ");
        }
    }

// All good, sleep for full period
    sleepTime = SLEEP_TIME_FULL;
}

// loop() does the cleanup and goes to sleep
void loop() {
    LoRa.end();
    digitalWrite(LORA_POWER, LOW);
    uint64_t totalTime = (millis() - processTime + 1) * 1000;
    Serial.printf("Processing time %d ms\n", millis() - processTime);
    Serial.flush();

    esp_sleep_enable_timer_wakeup(sleepTime - totalTime);
    esp_deep_sleep_start();
}
