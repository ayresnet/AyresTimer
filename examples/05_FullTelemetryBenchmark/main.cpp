/*
 * AyresTimer - PlatformIO Example 05: Full Telemetry & Benchmark
 * Developed by AyresNet (https://ayresnet.com)
 */

#include <Arduino.h>
#include <AyresTimer.h>

AyresTimerHW hwTimer(AyresTimerHW::TG0, AyresTimerHW::T0);
AyresTimerSW swTimer("BenchmarkSW");

volatile uint32_t hwCount = 0;
volatile uint32_t swCount = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n============================================================");
    Serial.println("  AYRES TIMER - PLATFORMIO: TELEMETRIA Y BENCHMARK ESP32    ");
    Serial.println("============================================================");
    Serial.printf("[INFO] Frecuencia de CPU: %u MHz\n", AyresTimerSW::cpuFreqMHz());
    Serial.printf("[INFO] Timer HW: TG0-T0 (100 ms) | Timer SW: esp_timer (250 ms)\n\n");

    hwTimer.setCallback([]() { hwCount++; });
    swTimer.setCallback([]() { swCount++; });

    hwTimer.periodicMs(100);
    swTimer.periodicMs(250);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));

    TimerStats hwStats = hwTimer.getStats();
    TimerStats swStats = swTimer.getStats();

    Serial.println("---------------- [ REPORTE DE TELEMETRIA ] ----------------");
    Serial.printf("[HW 100ms] Ticks: %-5u | Jitter: %+3lld us | MaxJitter: %3lld us | Restante: %2u ms\n",
                  hwCount, hwStats.lastJitterUs, hwStats.maxJitterUs, hwTimer.remainingMs());

    Serial.printf("[SW 250ms] Ticks: %-5u | Jitter: %+3lld us | MaxJitter: %3lld us | Progreso: %3.0f%%\n",
                  swCount, swStats.lastJitterUs, swStats.maxJitterUs, swTimer.progress() * 100.0f);
    Serial.println("-----------------------------------------------------------");
}
