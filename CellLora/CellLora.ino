#include "esp_sleep.h"
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Esp.h>

char VERSION[20] = "CellLora v100";

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

#define SLEEP_SECONDS_FULL  3600 // Every hour
#define SLEEP_SECONDS_RETRY 30 // This should be 60 normally
unsigned int sleepSeconds;
#define ID_HEADER    0x92
#define ID_CELLAR    0xCE

// Send LoRa packet
bool sendPacket(byte *payload, int size) {
    if (LoRa.beginPacket() == 0) {
        Serial.println("LoRa can't start packet");
        return false;
    } else {
        LoRa.write(ID_HEADER);
        LoRa.write(ID_CELLAR);

        for (int i = 0; i < size; i++) {
            LoRa.write(payload[i]);
        }

        if (LoRa.endPacket() == 0) {
            Serial.println("can't send packet");
            return false;
        }
    }
    
    return true;
}

// Everything happens in setup()

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
        LoRa.setTxPower(20);
        LoRa.setSpreadingFactor(12);
        LoRa.setSignalBandwidth(31.25E3);
        LoRa.setCodingRate4(8);
        Serial.println("LoRa init OK");
    }

// Read battery voltage    
    pinMode(PIN_ADC, ANALOG);

    int mV = 0;
    for (int i = 0; i < 11; i++) {
        mV += analogReadMilliVolts(PIN_ADC);
        delay(10);
    }
    float voltage = mV / 1000.0;
    Serial.printf("Battery %.1fV\n", voltage);

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

// Build LoRa packet
    byte payload[4];

    int battery = int(voltage * 10.0 + 0.5);
    payload[0] = battery;

    int temp_dec = int(temperature * 10.0 + 0.5);
    int temp_int = temp_dec / 10;
    temp_dec -= temp_int * 10;
    payload[1] = temp_int;
    payload[2] = temp_dec;

    int humi_int = int(humidity);
    payload[3] = humi_int;

// Send LoRa packet 3 times
    Serial.println("Send packet #1");
    sendPacket(payload, 4);
    delay(1000);
    Serial.println("Send packet #2");
    sendPacket(payload, 4);
    delay(1000);
    Serial.println("Send packet #3");
    sendPacket(payload, 4);

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
