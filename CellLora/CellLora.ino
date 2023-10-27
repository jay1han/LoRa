#include "esp_sleep.h"
#include "driver/uart.h"
#include <SPI.h>
#include <LoRa.h>

#define PIN_ADC    GPIO_NUM_3
#define PIN_SW1    GPIO_NUM_5
#define PIN_SW2    GPIO_NUM_0
#define PIN_LED    GPIO_NUM_15
#define PIN_LORA   GPIO_NUM_11
#define PIN_RST    GPIO_NUM_1
#define PIN_SS     GPIO_NUM_2
#define PIN_SCK    GPIO_NUM_4
#define PIN_MOSI   GPIO_NUM_6
#define PIN_MISO   GPIO_NUM_8
#define PIN_INT    GPIO_NUM_10

#define SLEEP_TIME (12 * 3600 * 1000000)
#define PIN_MASK   ((1 << PIN_SW1) | (1 << PIN_SW2))

bool loraInitOK = false;

void setup() {
    int time = millis();
    uart_set_pin(UART_NUM_0, 39, 37, 40, 38);
    Serial.begin(115200);
    pinMode(PIN_SW1, INPUT);
    pinMode(PIN_SW2, INPUT);

    bool mail = false;
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        Serial.print("TIMER  ");
    } else {
        if (digitalRead(PIN_SW1) == LOW && digitalRead(PIN_SW2) == LOW) {
            Serial.println("Switches ready");
            Serial.flush();
            esp_sleep_enable_ext1_wakeup(PIN_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
            esp_sleep_enable_timer_wakeup(SLEEP_TIME);
            esp_deep_sleep_start();
        } else {
            Serial.print("SWITCH ");
            digitalWrite(PIN_LED, HIGH);
            mail = true;
        }
    }

    pinMode(PIN_LORA, OUTPUT);
    digitalWrite(PIN_LORA, HIGH);
    pinMode(PIN_ADC, ANALOG);
    adcAttachPin(PIN_ADC);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_INT, INPUT);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
    LoRa.setPins(PIN_SS, PIN_RST, PIN_INT);
    if (LoRa.begin(433E6) == 0) {
        Serial.println("LoRa begin fail");
    } else {
        LoRa.setSignalBandwidth(7.8E3);
        LoRa.setSpreadingFactor(12);
        LoRa.enableCrc();
        loraInitOK = true;
    }

    int mV = 0;
    for (int i = 0; i < 11; i++) {
        mV += analogReadMilliVolts(PIN_ADC);
        delay(10);
    }
    float Voltage = mV / 1000.0;
    Serial.printf("%.2fV ", Voltage);

    if (loraInitOK) {
        if (LoRa.beginPacket() == 0) {
            Serial.println("\nLoRa can't start packet");
        } else {
            LoRa.printf("M=%c V=%.2f", mail ? 'Y' : 'N', Voltage);
            if (LoRa.endPacket() == 0) {
                Serial.println("\nLoRa can't finish packet");
            } else {
                Serial.println("Sent ");
            }
        }
    }

    LoRa.end()
    digitalWrite(PIN_LORA, LOW);
    digitalWrite(PIN_LED, LOW);
    Serial.printf("%d ms\n", millis() - time);
    Serial.flush();

    if (digitalRead(PIN_SW1) == LOW && digitalRead(PIN_SW2) == LOW) {
        esp_sleep_enable_ext1_wakeup(PIN_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_sleep_enable_timer_wakeup(SLEEP_TIME);
        esp_deep_sleep_start();
    } else {
        esp_sleep_enable_ext1_wakeup(PIN_MASK, ESP_EXT1_WAKEUP_ALL_LOW);
        esp_sleep_enable_timer_wakeup(SLEEP_TIME);
        esp_deep_sleep_start();
    }
}

void loop() {
}
