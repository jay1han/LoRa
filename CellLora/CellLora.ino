#include "esp_sleep.h"
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Esp.h>
#include "FFat.h"

char VERSION[20] = "CellLora v002";

#define UART_TX    21
#define UART_RX    2      // with pull-up for strapping
#define PIN_ADC    4
#define I2C_SDA    0
#define I2C_SCL    1
#define I2C_AHT10  0x38

#define LORA_POWER 3
#define LORA_MOSI  10
#define LORA_SS    20
#define LORA_MISO  5
#define LORA_SCK   6
#define LORA_RST   7
#define LORA_INT   -1

#define HISTORY_CSV "/history.csv"
#define HISTORY_OLD "/history.old"
#define HISTORY_NEW "/history.new"

#define SLEEP_SECONDS_FULL  300
#define SLEEP_SECONDS_RETRY 60
unsigned int sleepSeconds;
#define MAX_HISTORY    5
typedef struct {
    time_t time;
    float temperature;
    float humidity;
} HistoryItem;
#define ID_HEADER    0x92
#define ID_CELLAR    0xCE

// Everything happens in setup()

void writeItemLoRa(HistoryItem item) {
    byte temperature = 128, humidity = 0;
    
    int iTemp = int(item.temperature + 0.5);
    if (iTemp < 0)
        temperature = 255 - iTemp;
    else
        temperature = iTemp;
    humidity = int(item.humidity);
    
    LoRa.write(item.time >> 24);
    LoRa.write((item.time >> 16) & 0xFF);
    LoRa.write((item.time >> 8) & 0xFF);
    LoRa.write(item.time & 0xFF);
    LoRa.write(temperature);
    LoRa.write(humidity);
}

void writeHeaderLoRa(int pageCount, int battery) {
    Serial.printf("LoRa page %d ", pageCount);
    LoRa.write(ID_HEADER);
    LoRa.write(ID_CELLAR);
    LoRa.write(pageCount);
    LoRa.write(battery);
}

unsigned long processTime;
void setup() {
    processTime = millis();
    sleepSeconds = SLEEP_SECONDS_RETRY; // In case something goes wrong
    Serial.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
    Serial.println(VERSION);

// Initialize LoRa module
    pinMode(LORA_POWER, OUTPUT);
    digitalWrite(LORA_POWER, HIGH);
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    LoRa.setPins(LORA_SS, LORA_RST);
    if (LoRa.begin(433E6) == 0) {
        Serial.println("LoRa begin fail");
        return;
    } else {
        LoRa.setSpreadingFactor(10);
        LoRa.setSignalBandwidth(125E3);
        LoRa.setCodingRate4(8);
        Serial.println("LoRa init OK");
    }

// Read battery voltage    
    pinMode(PIN_ADC, ANALOG);
    adcAttachPin(PIN_ADC);

    int mV = 0;
    for (int i = 0; i < 11; i++) {
        mV += analogReadMilliVolts(PIN_ADC);
        delay(10);
    }
    float voltage = mV / 1000.0;
    Serial.printf("Battery %.1fV\n", voltage);
    int battery = int(voltage * 10.5);

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
    int length = 0;
    byte buffer[6];
    float temperature, humidity;
    
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
    File updateFile  = FFat.open(HISTORY_NEW, FILE_WRITE, true);

    if (updateFile) {
        Serial.print("Updating file .");
        updateFile.write((byte*)&thisItem, sizeof(HistoryItem));

        File historyFile = FFat.open(HISTORY_CSV, FILE_READ);
        if (historyFile) {
            int historySize = 1;
            HistoryItem backupItem;
            while(historyFile.available() && historySize < MAX_HISTORY) {
                historyFile.read((byte*)&backupItem, sizeof(HistoryItem));
                updateFile.write((byte*)&backupItem, sizeof(HistoryItem));
                historySize ++;
                Serial.print(".");
            }
            Serial.printf(" %d records\n", historySize);
            historyFile.close();
        } else {
            Serial.println("Can't open history file, continuing");
        }
        updateFile.close();

        FFat.rename(HISTORY_CSV, HISTORY_OLD);
        FFat.rename(HISTORY_NEW, HISTORY_CSV);
        FFat.remove(HISTORY_OLD);
    } else {
        Serial.println("Can't open update file, stopping");
        return;
    }

// Send LoRa packet
    if (LoRa.beginPacket() == 0) {
        Serial.println("LoRa can't start packet");
        return;
    } else {
        File historyFile = FFat.open(HISTORY_CSV, FILE_READ);
        HistoryItem historyItem;
        byte temperature, humidity;
        unsigned long time;
        int itemCount = 0, pageCount = 0;

        writeHeaderLoRa(pageCount, battery);

        while(historyFile.available()) {
            historyFile.read((byte*)&historyItem, sizeof(HistoryItem));
            writeItemLoRa(historyItem);
            Serial.print(".");
            itemCount ++;

            // Start a new packet after 24 hours of history
            if (itemCount == 24) {
                if (LoRa.endPacket() == 0) {
                    Serial.println("can't send packet");
                    return;
                } else {
                    Serial.printf(" %d items\n", itemCount);
                    itemCount = 0;
                    pageCount ++;
                    if (historyFile.available()) {
                        if (LoRa.beginPacket() == 0) {
                            Serial.println("Can't start next packet");
                            return;
                        } else {
                            writeHeaderLoRa(pageCount, battery);
                            writeItemLoRa(thisItem);
                        }
                    }
                }
            }
        }
        historyFile.close();
        
        // All done, send final packet
        if (itemCount > 0) {
            if (LoRa.endPacket() == 0) {
                Serial.println("can't finish packet");
                return;
            } else {
                Serial.printf(" %d items. END\n", itemCount);
            }
        }
    }

// All good, sleep for full period
    sleepSeconds = SLEEP_SECONDS_FULL;
}

// loop() does the cleanup and goes to sleep
void loop() {
    LoRa.sleep();
    digitalWrite(LORA_POWER, LOW);
    uint64_t sleepTime = (sleepSeconds * 1000000L) - (millis() - processTime + 1) * 1000L;
    Serial.printf("Processing time %d ms, sleep for %ld seconds\n",
                  millis() - processTime, sleepTime / 1000000);
    Serial.flush();

    esp_sleep_enable_timer_wakeup(sleepTime);
    esp_deep_sleep_start();
}
