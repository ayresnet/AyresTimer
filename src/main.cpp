/*
 * AyresTimer v4.1.0 - Banco de Pruebas y Demostración "Nivel Espacial"
 *
 * Muestra el funcionamiento asíncrono, medición de jitter, control de pausas,
 * retriggering para alarmas/watchdog y cronometraje de alta precisión sin millis().
 */

#include <Arduino.h>
#include "AyresTimer.h"

// Instancias de demostración
AyresTimerHW hwTimer(AyresTimerHW::TG0, AyresTimerHW::T0);
AyresTimerSW swHeartbeat("Heartbeat");
AyresTimerSW alarmWatchdog("Watchdog");

AyresTimeout zoneTimeout;
AyresStopwatch benchStopwatch;

// Variables de estado atómicas / volátiles para la telemetría en tiempo real
volatile uint32_t hwTickCount = 0;
volatile uint32_t swTickCount = 0;
volatile uint32_t watchdogAlarmCount = 0;

void setup() {
    Serial.begin(115200);
    delay(1000); // Pequeño retardo para estabilizar el puerto serie

    Serial.println("\n=================================================================");
    Serial.println("   AYRES TIMER v4.1.0 - SUITE DE TEMPORIZACION ESPACIAL / PRO    ");
    Serial.println("=================================================================");
    Serial.printf("[INFO] Frecuencia de CPU: %u MHz\n", AyresTimerSW::cpuFreqMHz());
    Serial.printf("[INFO] Base de tiempo: esp_timer 64-bit microsegundos (sin rollover)\n");
    Serial.println("-----------------------------------------------------------------");

    // -------------------------------------------------------------------------
    // 1. Configuración de Timer por Hardware (Alta Velocidad / 100 ms)
    // -------------------------------------------------------------------------
    hwTimer.setCallback([]() {
        hwTickCount++;
    });
    // Iniciar periódico cada 100 ms (100,000 us)
    if (hwTimer.periodicMs(100)) {
        Serial.println("[OK] AyresTimerHW (TG0-T0) iniciado periodicamente a 100 ms.");
    } else {
        Serial.printf("[ERROR] AyresTimerHW fallo: %s\n", hwTimer.lastErrorStr());
    }

    // -------------------------------------------------------------------------
    // 2. Configuración de Timer por Software (Heartbeat / 500 ms)
    // -------------------------------------------------------------------------
    swHeartbeat.setCallback([]() {
        swTickCount++;
    });
    if (swHeartbeat.periodicMs(500)) {
        Serial.println("[OK] AyresTimerSW (Heartbeat) iniciado periodicamente a 500 ms.");
    }

    // -------------------------------------------------------------------------
    // 3. Configuración de Timer Retriggerable (Watchdog / Zona de iAlarma a 3 seg)
    // -------------------------------------------------------------------------
    alarmWatchdog.setCallback([]() {
        watchdogAlarmCount++;
        Serial.println("\n>>> [ALERTA] WATCHDOG EXPIRO: Zona de alarma activada sin rearme! <<<");
    });
    if (alarmWatchdog.retriggerableSec(3)) {
        Serial.println("[OK] AyresTimerSW (Watchdog de Alarma) armado a 3.0 segundos.");
    }

    // -------------------------------------------------------------------------
    // 4. Configuración de AyresTimeout ligero (Máquina de estados a 5 segundos)
    // -------------------------------------------------------------------------
    zoneTimeout.setSec(5);
    Serial.println("[OK] AyresTimeout (Estado de Zona) configurado a 5.0 segundos.");

    Serial.println("-----------------------------------------------------------------");
    Serial.println("Iniciando lazo de telemetria en tiempo real...");
    Serial.println("=================================================================\n");
}

void loop() {
    static uint64_t lastPrintUs = 0;
    const uint64_t nowUs = AyresTimerSW::micros64();

    // Telemetría cada 1 segundo sin usar millis()
    if (nowUs - lastPrintUs >= 1000000ULL) {
        lastPrintUs = nowUs;

        // Estadísticas de Hardware Timer
        TimerStats hwStats = hwTimer.getStats();
        TimerStats swStats = swHeartbeat.getStats();

        Serial.println("\n----------------- [ TELEMETRIA EN TIEMPO REAL ] -----------------");
        Serial.printf("[AyresTimerHW]  Ticks: %-5u | Disparos: %-5llu | Jitter: %+4lld us | MaxJitter: %4lld us | Restante: %4u ms\n",
                      hwTickCount,
                      hwStats.totalTriggers,
                      hwStats.lastJitterUs,
                      hwStats.maxJitterUs,
                      hwTimer.remainingMs());

        Serial.printf("[AyresTimerSW]  Ticks: %-5u | Disparos: %-5llu | Jitter: %+4lld us | MaxJitter: %4lld us | Progreso: %3.0f%%\n",
                      swTickCount,
                      swStats.totalTriggers,
                      swStats.lastJitterUs,
                      swStats.maxJitterUs,
                      swHeartbeat.progress() * 100.0f);

        Serial.printf("[Watchdog]      Restante para disparo: %4u ms | Disparos de Alarma: %u\n",
                      alarmWatchdog.remainingMs(),
                      watchdogAlarmCount);

        Serial.printf("[AyresTimeout]  Transcurrido: %4u ms | Restante: %4u ms | Progreso: %3.0f%% | Expirado: %s\n",
                      zoneTimeout.elapsedMs(),
                      zoneTimeout.remainingMs(),
                      zoneTimeout.progress() * 100.0f,
                      zoneTimeout.isExpired() ? "SI" : "NO");

        // Simulación periódica de rearme / retrigger de presencia cada 2 segundos
        static int simCounter = 0;
        simCounter++;

        if (simCounter % 2 == 0 && simCounter <= 6) {
            Serial.println("[EVENTO] Sensor detecto movimiento -> Re-armando Watchdog con alarmWatchdog.retrigger()");
            alarmWatchdog.retrigger();
        }

        // Demostración de Pausa / Reanudación
        if (simCounter == 8) {
            Serial.println("[EVENTO] Pausando AyresTimerHW con hwTimer.pause()...");
            hwTimer.pause();
        } else if (simCounter == 11) {
            Serial.println("[EVENTO] Reanudando AyresTimerHW con hwTimer.resume()...");
            hwTimer.resume();
        }

        // Reiniciar el timeout ligero
        if (zoneTimeout.isExpired()) {
            Serial.println("[EVENTO] AyresTimeout expiro -> Reiniciando temporizador de zona a 5 seg...");
            zoneTimeout.restart();
        }

        // Demostración de AyresStopwatch cronometrando una operación
        benchStopwatch.start();
        AyresTimerSW::busyWaitUs(1500); // Simular tarea de 1.5 ms
        benchStopwatch.pause();
        Serial.printf("[Benchmark]     Tiempo medido con AyresStopwatch: %llu us (%.3f ms)\n",
                      benchStopwatch.elapsedUs(),
                      benchStopwatch.elapsedSec() * 1000.0f);

        Serial.println("-----------------------------------------------------------------");
    }

    // Cero retrasos bloqueantes en loop()
    vTaskDelay(pdMS_TO_TICKS(10));
}