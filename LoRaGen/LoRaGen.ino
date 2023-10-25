#include <SPI.h>
#include <LoRa.h>
#include <Esp.h>
#include <EasyNeoPixels.h>
#include <Wire.h>

#define PIN_LED      7   // one NeoPixel
#define LED_POWER    80
#define LED_POWER2   40
#define LED_POWER3   20

#define LORA_SS      3
#define LORA_RST     0
#define LORA_SCK     4
#define LORA_MOSI    10
#define LORA_MISO    8
#define LORA_INT     6

#define ID_HEADER    0x92
#define ID_BAL       0xBA
#define ID_CELLAR    0xCE
#define ID_TEST      0xA5

#define I2C_AHT10    0x38
#define I2C_SCL      5
#define I2C_SDA      1

void aht10Init() {
    Wire.begin(I2C_SDA, I2C_SCL, 400000);
    
    Serial.print("AHT10 Init");
    Wire.beginTransmission(I2C_AHT10);
    Wire.write(0xE1);
    Wire.write(0x08);
    Wire.write(0x00);
    if (Wire.endTransmission() == 0) {
        Serial.print("...");
        if (Wire.requestFrom(I2C_AHT10, 1) == 1) {
            byte status = Wire.read();
            Serial.printf(" Status=0x%02X", status);
            if ((status & 0x68) == 0x08) {
                Serial.println(" OK");
                return;
            }
        }
    }
    Serial.println(" failed");
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_LED, OUTPUT);
    setupEasyNeoPixels(PIN_LED, 1);
    writeEasyNeoPixel(0, 0, LED_POWER, 0);

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
    }

    aht10Init();

    writeEasyNeoPixel(0, 0, 0, LED_POWER);
}

#define NUM_TESTS 3
typedef struct test_t {
    byte header;
    void (*maker)(test_t &);
    int size;
    byte data[200];
} Test;
void makeBal(Test &test);
void makeCellar(Test &test);
void makeTest(Test &test);
Test Tests[NUM_TESTS] = {
    {ID_BAL, makeBal, 3, {}},
    {ID_CELLAR, makeCellar, 24 * 3 * 2, {}},
    {ID_TEST, makeTest, 0, {}}
};

int step = 1;
void loop() {
    writeEasyNeoPixel(0, LED_POWER3, LED_POWER2, 0);
    Serial.printf("Sending %d\n", step);

    for (int i = 0; i < NUM_TESTS; i++) {
        Serial.printf("Sending test item %d... ", i);
        Test test = Tests[i];
        test.maker(test);
        
        unsigned int txTime = millis();
        if (LoRa.beginPacket() == 0) {
            Serial.println("can't start packet");
        } else {
            LoRa.write(ID_HEADER);
            LoRa.write(test.header);
            for (int j = 0; j < test.size; j++) {
                LoRa.write(test.data[j]);
            }
            if (LoRa.endPacket() == 0) {
                Serial.println("can't finish packet");
            } else {
                Serial.printf("Sent in %dms\n", millis() - txTime);
            }
        }
    }
    
    writeEasyNeoPixel(0, 0, 0, LED_POWER);
    step ++;
}

void makeTest(Test &test) {
    float temperature, humidity;
    aht10Read(&temperature, &humidity);
    char message[40];
    sprintf(message, "Temp=%.2f Hum=%.1f", temperature, humidity);
    memcpy(test.data, message, strlen(message));
    test.size = strlen(message) + 1;
}

void makeBal(Test &test) {
    test.data[0] = random(2);
    test.data[1] = random(4);
    test.data[2] = random(100);
}

void makeCellar(Test &test) {
    for (int i = 0; i < test.size / 2; i++) {
        test.data[2 * i] = random(70) - 20;
        test.data[2 * i + 1] = random(100);
    }
}

void aht10Read(float *temperature, float *humidity) {
    *temperature = *humidity = 0.0;
    int length = 0;
    byte buffer[6];
    
    Serial.print("AHT10 read data... ");
    unsigned int aht10Time = millis();
    Wire.beginTransmission(I2C_AHT10);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
        Serial.println("send command failed");
        return;
    }
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

    unsigned int rawHum = ((buffer[1] << 16) | (buffer[2] << 8) | buffer[3]) >> 4;
    unsigned int rawTemp  = ((buffer[3] & 0x0F) << 16) | (buffer[4] << 8) | buffer[5];
    float floatHum  = (100.0 * (float)rawHum) / (1 << 20);
    float floatTemp = ((200.0 * (float)rawTemp) / (1 << 20)) - 50.0;
    Serial.printf("Temp=%d=%.2f Hum=%d=%.2f ", rawTemp, floatTemp, rawHum, floatHum);

    *temperature = floatTemp;
    *humidity    = floatHum;
}
