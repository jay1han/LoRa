#include <SPI.h>
#include <LoRa.h>
#include <Esp.h>
#include <EasyNeoPixels.h>

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

    writeEasyNeoPixel(0, 0, 0, LED_POWER);
}

#define NUM_TESTS 2
typedef struct {
    byte header;
    int size;
    byte data[200];
} Test;
Test Tests[NUM_TESTS] = {
    {ID_BAL, 3,
     {1, 4, 0}},
    {ID_CELLAR, 160,
     {10, 80, 11, 78, 12, 76, 13, 74, 14, 72, 15, 70, 16, 68, 17, 66, 18, 64, 19, 62,
      20, 58, 21, 54, 22, 50, 18, 46, 14, 42, 10, 40, 12, 40, 10, 40, 10, 50, 10, 60,
      10, 80, 11, 78, 12, 76, 13, 74, 14, 72, 15, 70, 16, 68, 17, 66, 18, 64, 19, 62,
      20, 58, 21, 54, 22, 50, 18, 46, 14, 42, 10, 40, 12, 40, 10, 40, 10, 50, 10, 60,
      10, 80, 11, 78, 12, 76, 13, 74, 14, 72, 15, 70, 16, 68, 17, 66, 18, 64, 19, 62,
      20, 58, 21, 54, 22, 50, 18, 46, 14, 42, 10, 40, 12, 40, 10, 40, 10, 50, 10, 60,
      10, 80, 11, 78, 12, 76, 13, 74, 14, 72, 15, 70, 16, 68, 17, 66, 18, 64, 19, 62,
      20, 58, 21, 54, 22, 50, 18, 46, 14, 42, 10, 40, 12, 40, 10, 40, 10, 50, 10, 60}}
};

int step = 1;
void loop() {
    writeEasyNeoPixel(0, LED_POWER3, LED_POWER2, 0);
    Serial.printf("Sending %d\n", step);
    unsigned int txTime = millis();

    for (int i = 0; i < NUM_TESTS; i++) {
        Serial.printf("Sending test item %d... ", i);
        if (LoRa.beginPacket() == 0) {
            Serial.println("can't start packet");
        } else {
            Test test = Tests[i];
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
