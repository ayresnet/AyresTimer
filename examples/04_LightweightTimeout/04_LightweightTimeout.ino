/*
 * AyresTimer - Ejemplo 04: Temporizador Ligero de Estado (AyresTimeout)
 *
 * Muestra el uso de AyresTimeout y AyresStopwatch para máquinas de estado,
 * retardos no bloqueantes y medición de tiempo de ejecución con cero consumo de tareas.
 */

#include <Arduino.h>
#include <AyresTimer.h>

AyresTimeout zoneTimeout;
AyresStopwatch benchStopwatch;

enum AlarmState {
    STANDBY,
    ENTRY_DELAY,
    ALARM_TRIGGERED
};

AlarmState currentState = STANDBY;

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n--- [AyresTimer] Ejemplo 04: Lightweight Timeout & Stopwatch ---");
    Serial.println("[ESTADO] Sistema en STANDBY. Simulando apertura de puerta en 2 segundos...");
}

void loop() {
    static int loopCount = 0;
    loopCount++;
    vTaskDelay(pdMS_TO_TICKS(500));

    // Simulación de máquina de estados
    switch (currentState) {
        case STANDBY:
            if (loopCount >= 4) { // A los 2 segundos
                Serial.println("\n[EVENTO] ¡Puerta abierta! Iniciando retardo de entrada de 5 segundos...");
                zoneTimeout.setSec(5); // 5 segundos para desarmar
                currentState = ENTRY_DELAY;
            }
            break;

        case ENTRY_DELAY:
            Serial.printf("[CUENTA ATRAS] Tiempo restante para desarmar: %4u ms (Progreso: %3.0f%%)\n",
                          zoneTimeout.remainingMs(),
                          zoneTimeout.progress() * 100.0f);

            if (zoneTimeout.isExpired()) {
                Serial.println("\n>>> [SIRENA ACTIVADA] No se ingreso el codigo a tiempo! <<<\n");
                currentState = ALARM_TRIGGERED;
            }
            break;

        case ALARM_TRIGGERED:
            // Medir con AyresStopwatch cuánto tarda una función de procesamiento
            benchStopwatch.start();
            AyresTimerSW::busyWaitUs(2500); // Tarea simulada de 2.5 ms
            benchStopwatch.pause();

            Serial.printf("[Benchmark] Tiempo de procesamiento de alarma: %llu microsegundos\n",
                          benchStopwatch.elapsedUs());
            vTaskDelay(pdMS_TO_TICKS(1500));
            break;
    }
}
