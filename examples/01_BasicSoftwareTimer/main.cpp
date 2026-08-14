/*
 * AyresTimer - PlatformIO Example 01: Basic Software Timer
 * Developed by AyresNet (https://ayresnet.com)
 */

#include <Arduino.h>
#include <AyresTimer.h>

// Instancias de temporizadores por Software
AyresTimerSW ledTimer("LedTimer");
AyresTimerSW oneShotTimer("OneShot");

static constexpr int LED_PIN = 2; // LED integrado en la placa ESP32

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- [AyresTimer - PlatformIO] Ejemplo 01: Basic Software Timer ---");
    pinMode(LED_PIN, OUTPUT);

    // 1. Temporizador periódico para alternar el LED cada 500 ms
    ledTimer.setCallback([]() {
        static bool state = false;
        state = !state;
        digitalWrite(LED_PIN, state ? HIGH : LOW);
        Serial.printf("[LedTimer] Estado del LED: %s (Core %d)\n", state ? "ENCENDIDO" : "APAGADO", xPortGetCoreID());
    });
    ledTimer.periodicMs(500);

    // 2. Temporizador One-Shot que se ejecutará una sola vez a los 5 segundos
    oneShotTimer.setCallback([]() {
        Serial.println("\n>>> [OneShotTimer] ¡Disparo único ejecutado a los 5 segundos! <<<\n");
    });
    oneShotTimer.oneShotSec(5);
}

void loop() {
    // El loop principal queda libre para la aplicación sin bloqueos
    vTaskDelay(pdMS_TO_TICKS(1000));
}
