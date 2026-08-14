/*
 * AyresTimer - PlatformIO Example 03: Retriggerable Watchdog (Alarms & Presence)
 * Developed by AyresNet (https://ayresnet.com)
 */

#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerSW watchdog("Watchdog");

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- [AyresTimer - PlatformIO] Ejemplo 03: Retriggerable Watchdog ---");

    // Callback de alerta si se agota el tiempo sin rearme
    watchdog.setCallback([]() {
        Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
        Serial.println(">>> [ALERTA DE SEGURIDAD] ¡TIEMPO DE VIGILANCIA AGOTADO! <<<");
        Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    });

    // Ventana de rearme de 3.0 segundos
    watchdog.retriggerableSec(3);
    Serial.println("[OK] Watchdog armado con ventana de 3.0 segundos.");
}

void loop() {
    static int seconds = 0;
    seconds++;

    vTaskDelay(pdMS_TO_TICKS(1000));

    Serial.printf("[Tiempo %2d s] Tiempo restante para disparo: %4u ms\n",
                  seconds, watchdog.remainingMs());

    // Simulación de sensor de presencia en los primeros 6 segundos
    if (seconds <= 6 && seconds % 2 == 0) {
        Serial.println(" -> [SENSOR] Movimiento detectado: Re-armando con watchdog.retrigger()");
        watchdog.retrigger();
    } else if (seconds == 7) {
        Serial.println(" -> [SENSOR] Se detuvo el movimiento. El watchdog expirara pronto...");
    }
}
