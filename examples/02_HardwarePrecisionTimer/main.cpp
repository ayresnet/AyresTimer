/*
 * AyresTimer - PlatformIO Example 02: Hardware Precision Timer (Timer Groups TG0/TG1)
 * Developed by AyresNet (https://ayresnet.com)
 */

#include <Arduino.h>
#include <AyresTimer.h>

// Temporizador de Hardware Grupo 0, Timer 0
AyresTimerHW hwTimer(AyresTimerHW::TG0, AyresTimerHW::T0);

volatile uint32_t pulseCount = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- [AyresTimer - PlatformIO] Ejemplo 02: Hardware Precision Timer ---");

    // Callback C++ que corre en una Task FreeRTOS dedicada de forma segura
    hwTimer.setCallback([]() {
        pulseCount++;
    });

    // Iniciar temporizador periódico a 100 ms (100,000 us)
    if (hwTimer.periodicMs(100)) {
        Serial.println("[OK] Timer por Hardware iniciado a 100 ms.");
    } else {
        Serial.printf("[ERROR] Fallo al iniciar: %s\n", hwTimer.lastErrorStr());
    }
}

void loop() {
    static int cycle = 0;
    cycle++;

    vTaskDelay(pdMS_TO_TICKS(1000));

    TimerStats stats = hwTimer.getStats();
    Serial.printf("[Ciclo %2d] Pulsos: %-5u | Jitter: %+3lld us | MaxJitter: %3lld us | Restante: %2u ms\n",
                  cycle,
                  pulseCount,
                  stats.lastJitterUs,
                  stats.maxJitterUs,
                  hwTimer.remainingMs());

    // Demostración de pausa y reanudación
    if (cycle == 5) {
        Serial.println(">>> Pausando timer de hardware (hwTimer.pause()) <<<");
        hwTimer.pause();
    } else if (cycle == 8) {
        Serial.println(">>> Reanudando timer de hardware (hwTimer.resume()) <<<");
        hwTimer.resume();
    }
}
