/*
 * AyresTimer - Ejemplo 01: Temporizador Básico por Software (esp_timer)
 *
 * Muestra el uso de temporizadores periódicos y de un solo disparo (one-shot)
 * despachados de forma segura en tareas FreeRTOS sin bloquear el loop().
 */

#include <Arduino.h>
#include <AyresTimer.h>

// Instancias de temporizadores
AyresTimerSW ledTimer("LedTimer");
AyresTimerSW oneShotTimer("OneShot");

const int LED_PIN = 2; // LED integrado en la mayoría de placas ESP32

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- [AyresTimer] Ejemplo 01: Basic Software Timer ---");
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
    // El loop queda completamente libre para otras tareas de tu aplicación
    vTaskDelay(pdMS_TO_TICKS(1000));
}
