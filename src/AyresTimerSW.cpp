/* AyresTimer v4.1.0 - Software backend (esp_timer / High Resolution Timer). */

#include "AyresTimer.h"

#include <esp_timer.h>
#include <esp_err.h>
#include <esp_attr.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr TickType_t kTaskShutdownPollTicks = pdMS_TO_TICKS(1);

class RecursiveGuard {
public:
    explicit RecursiveGuard(SemaphoreHandle_t mutex) : _mutex(mutex) {
        _locked = _mutex && xSemaphoreTakeRecursive(_mutex, portMAX_DELAY) == pdTRUE;
    }
    ~RecursiveGuard() {
        if (_locked) xSemaphoreGiveRecursive(_mutex);
    }
    explicit operator bool() const { return _locked; }
private:
    SemaphoreHandle_t _mutex = nullptr;
    bool _locked = false;
};

bool validCore(BaseType_t core) {
    return core == AYRES_TIMER_AUTO_CORE || core == tskNO_AFFINITY ||
           (core >= 0 && core < portNUM_PROCESSORS);
}
}

AyresTimer<AYRES_SOFTWARE>::AyresTimer(const char* name) {
    snprintf(_name, sizeof(_name), "%s", (name && *name) ? name : "AyresTimer");
    _apiMutex = xSemaphoreCreateRecursiveMutex();
    _callbackMutex = xSemaphoreCreateRecursiveMutex();
    if (!_apiMutex || !_callbackMutex) {
        setError_(ESP_ERR_NO_MEM);
        return;
    }
    (void)recreateTimer_();
}

AyresTimer<AYRES_SOFTWARE>::~AyresTimer() {
    (void)stop();

    if (_handle) {
        const esp_err_t err = esp_timer_delete(_handle);
        if (err == ESP_OK) {
            _handle = nullptr;
        } else {
            setError_(err);
        }
    }

    stopTask_();

    if (_callbackMutex) {
        vSemaphoreDelete(_callbackMutex);
        _callbackMutex = nullptr;
    }
    if (_apiMutex) {
        vSemaphoreDelete(_apiMutex);
        _apiMutex = nullptr;
    }
}

bool AyresTimer<AYRES_SOFTWARE>::validMode_(TimerMode mode) const {
    return mode == ONE_SHOT || mode == PERIODIC || mode == RETRIGGERABLE;
}

void AyresTimer<AYRES_SOFTWARE>::setError_(esp_err_t error) const {
    portENTER_CRITICAL(&_mux);
    _lastError = error;
    portEXIT_CRITICAL(&_mux);
}

esp_err_t AyresTimer<AYRES_SOFTWARE>::lastError() const {
    portENTER_CRITICAL(&_mux);
    const esp_err_t result = _lastError;
    portEXIT_CRITICAL(&_mux);
    return result;
}

const char* AyresTimer<AYRES_SOFTWARE>::lastErrorStr() const {
    return esp_err_to_name(lastError());
}

TimerStats AyresTimer<AYRES_SOFTWARE>::getStats() const {
    portENTER_CRITICAL(&_mux);
    const TimerStats s = _stats;
    portEXIT_CRITICAL(&_mux);
    return s;
}

void AyresTimer<AYRES_SOFTWARE>::resetStats() {
    portENTER_CRITICAL(&_mux);
    _stats = TimerStats{};
    portEXIT_CRITICAL(&_mux);
}

bool AyresTimer<AYRES_SOFTWARE>::recreateTimer_() {
    if (_handle) {
        if (esp_timer_is_active(_handle)) {
            const esp_err_t stopErr = esp_timer_stop(_handle);
            if (stopErr != ESP_OK) {
                setError_(stopErr);
                return false;
            }
        }
        const esp_err_t deleteErr = esp_timer_delete(_handle);
        if (deleteErr != ESP_OK) {
            setError_(deleteErr);
            return false;
        }
        _handle = nullptr;
    }

    memset(&_args, 0, sizeof(_args));
    _args.callback = &AyresTimer<AYRES_SOFTWARE>::trampoline;
    _args.arg = this;
    _args.name = _name;

#if defined(CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD) && CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
    _args.dispatch_method = _dispatchInISR ? ESP_TIMER_ISR : ESP_TIMER_TASK;
#else
    _args.dispatch_method = ESP_TIMER_TASK;
#endif

    const esp_err_t err = esp_timer_create(&_args, &_handle);
    setError_(err);
    return err == ESP_OK;
}

bool AyresTimer<AYRES_SOFTWARE>::setDispatchInISR(bool enable) {
#if !(defined(CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD) && CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD)
    if (enable) {
        setError_(ESP_ERR_NOT_SUPPORTED);
        return false;
    }
#endif

    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    const bool unchanged = (_dispatchInISR == enable);
    const bool wasRunning = _running;
    const TimerMode previousMode = _mode;
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    if (unchanged) {
        const bool valid = _handle != nullptr;
        setError_(valid ? ESP_OK : ESP_ERR_INVALID_STATE);
        return valid;
    }

    if (!stop()) {
        return false;
    }

    portENTER_CRITICAL(&_mux);
    _dispatchInISR = enable;
    portEXIT_CRITICAL(&_mux);

    if (!recreateTimer_()) {
        return false;
    }

    if (wasRunning) {
        return start(timeout, previousMode);
    }
    setError_(ESP_OK);
    return true;
}

bool AyresTimer<AYRES_SOFTWARE>::start(uint64_t microseconds, TimerMode mode) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    if (microseconds == 0 || !validMode_(mode)) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    const bool hasCppCallback = _hasCppCallback;
    const bool dispatchInISR = _dispatchInISR;
    const bool userEnabledTask = _useTask;
    portEXIT_CRITICAL(&_mux);

    if (hasCppCallback && (dispatchInISR || userEnabledTask)) {
        if (!ensureTask_()) return false;
    }

    if (!stop()) return false;
    if (!_handle && !recreateTimer_()) return false;

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL(&_mux);
    _timeout_us = microseconds;
    _start_us = now;
    _mode = mode;
    _running = true;
    _paused = false;
    _paused_elapsed_us = 0;
    _expectedNextUs = now + microseconds;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);

    esp_err_t err = ESP_OK;
    if (mode == PERIODIC) {
        err = esp_timer_start_periodic(_handle, microseconds);
    } else {
        err = esp_timer_start_once(_handle, microseconds);
    }

    if (err != ESP_OK) {
        portENTER_CRITICAL(&_mux);
        _running = false;
        _lastError = err;
        portEXIT_CRITICAL(&_mux);
        return false;
    }
    return true;
}

bool AyresTimer<AYRES_SOFTWARE>::stop() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    esp_err_t err = ESP_OK;
    if (_handle && esp_timer_is_active(_handle)) {
        err = esp_timer_stop(_handle);
    }

    portENTER_CRITICAL(&_mux);
    _running = false;
    _paused = false;
    _paused_elapsed_us = 0;
    _cancelThrough = _producedCallbacks;
    _lastError = err;
    portEXIT_CRITICAL(&_mux);
    return err == ESP_OK;
}

bool AyresTimer<AYRES_SOFTWARE>::pause() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool canPause = _handle && _running && !_paused;
    portEXIT_CRITICAL(&_mux);
    if (!canPause) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    const uint64_t currentElapsed = elapsed();
    esp_err_t err = ESP_OK;
    if (esp_timer_is_active(_handle)) {
        err = esp_timer_stop(_handle);
    }

    if (err == ESP_OK) {
        portENTER_CRITICAL(&_mux);
        _paused = true;
        _paused_elapsed_us = currentElapsed;
        _lastError = ESP_OK;
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    setError_(err);
    return false;
}

bool AyresTimer<AYRES_SOFTWARE>::resume() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool canResume = _handle && _running && _paused;
    const uint64_t timeout = _timeout_us;
    const uint64_t pausedElapsed = _paused_elapsed_us;
    const TimerMode mode = _mode;
    portEXIT_CRITICAL(&_mux);
    if (!canResume) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    const uint64_t remainingUs = (pausedElapsed >= timeout) ? 1ULL : (timeout - pausedElapsed);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    esp_err_t err = esp_timer_start_once(_handle, remainingUs);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&_mux);
        _paused = false;
        _start_us = now - pausedElapsed;
        _expectedNextUs = now + remainingUs;
        _lastError = ESP_OK;
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    setError_(err);
    return false;
}

bool AyresTimer<AYRES_SOFTWARE>::restart() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    const TimerMode mode = _mode;
    portEXIT_CRITICAL(&_mux);
    if (timeout == 0) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    return start(timeout, mode);
}

bool AyresTimer<AYRES_SOFTWARE>::updatePeriod(uint64_t microseconds) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    if (microseconds == 0) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    const bool running = _running;
    const TimerMode mode = _mode;
    if (!running) {
        _timeout_us = microseconds;
        _lastError = ESP_OK;
    }
    portEXIT_CRITICAL(&_mux);
    return running ? start(microseconds, mode) : true;
}

bool AyresTimer<AYRES_SOFTWARE>::retrigger() {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool allowed = _running && _mode == RETRIGGERABLE && _timeout_us > 0;
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    if (!allowed) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    if (_handle && esp_timer_is_active(_handle)) {
        esp_timer_stop(_handle);
    }
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL(&_mux);
    _start_us = now;
    _expectedNextUs = now + timeout;
    portEXIT_CRITICAL(&_mux);

    const esp_err_t err = esp_timer_start_once(_handle, timeout);
    setError_(err);
    return err == ESP_OK;
}

uint64_t AyresTimer<AYRES_SOFTWARE>::elapsed() const {
    portENTER_CRITICAL(&_mux);
    const bool running = _running;
    const bool paused = _paused;
    const uint64_t pausedElapsed = _paused_elapsed_us;
    const uint64_t startUs = _start_us;
    const uint64_t timeout = _timeout_us;
    const TimerMode mode = _mode;
    portEXIT_CRITICAL(&_mux);
    if (!running) return 0;
    if (paused) return pausedElapsed;

    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t delta = (now >= startUs) ? (now - startUs) : 0;
    if (mode == PERIODIC && timeout > 0) {
        return delta % timeout;
    }
    return delta;
}

uint64_t AyresTimer<AYRES_SOFTWARE>::remaining() const {
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    const uint64_t el = elapsed();
    return (el >= timeout) ? 0 : (timeout - el);
}

float AyresTimer<AYRES_SOFTWARE>::progress() const {
    portENTER_CRITICAL(&_mux);
    const uint64_t timeout = _timeout_us;
    portEXIT_CRITICAL(&_mux);
    if (timeout == 0) return 1.0f;
    const uint64_t el = elapsed();
    if (el >= timeout) return 1.0f;
    return static_cast<float>(el) / static_cast<float>(timeout);
}

bool AyresTimer<AYRES_SOFTWARE>::isRunning() const {
    portENTER_CRITICAL(&_mux);
    const bool running = _running && !_paused;
    portEXIT_CRITICAL(&_mux);
    return running;
}

bool AyresTimer<AYRES_SOFTWARE>::isPaused() const {
    portENTER_CRITICAL(&_mux);
    const bool paused = _paused;
    portEXIT_CRITICAL(&_mux);
    return paused;
}

bool AyresTimer<AYRES_SOFTWARE>::setCallback(Callback callback) {
    if (!_callbackMutex || xPortInIsrContext()) {
        setError_(_callbackMutex ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool pendingCallback = _producedCallbacks != _consumedCallbacks ||
                                 _callbackActive;
    portEXIT_CRITICAL(&_mux);
    if (isRunning() || pendingCallback) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }

    xSemaphoreTakeRecursive(_callbackMutex, portMAX_DELAY);
    portENTER_CRITICAL(&_mux);
    const bool becameBusy = _running || _callbackActive ||
                            _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (becameBusy) {
        xSemaphoreGiveRecursive(_callbackMutex);
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    _callback = std::move(callback);
    const bool hasCallback = static_cast<bool>(_callback);
    xSemaphoreGiveRecursive(_callbackMutex);
    portENTER_CRITICAL(&_mux);
    _hasCppCallback = hasCallback;
    _isrCallback = nullptr;
    _isrArg = nullptr;
    portEXIT_CRITICAL(&_mux);

    setError_(ESP_OK);
    return true;
}

bool AyresTimer<AYRES_SOFTWARE>::setISRCallback(ISRCallback callback, void* arg) {
    if (xPortInIsrContext()) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool pendingCallback = _producedCallbacks != _consumedCallbacks ||
                                 _callbackActive;
    portEXIT_CRITICAL(&_mux);
    if (isRunning() || pendingCallback || !_callbackMutex) {
        setError_(_callbackMutex ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM);
        return false;
    }

    xSemaphoreTakeRecursive(_callbackMutex, portMAX_DELAY);
    portENTER_CRITICAL(&_mux);
    const bool becameBusy = _running || _callbackActive ||
                            _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (becameBusy) {
        xSemaphoreGiveRecursive(_callbackMutex);
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    _callback = nullptr;
    xSemaphoreGiveRecursive(_callbackMutex);
    portENTER_CRITICAL(&_mux);
    _hasCppCallback = false;
    _isrCallback = callback;
    _isrArg = arg;
    portEXIT_CRITICAL(&_mux);
    stopTask_();
    setError_(ESP_OK);
    return true;
}

bool AyresTimer<AYRES_SOFTWARE>::ensureTask_() {
    portENTER_CRITICAL(&_mux);
    if (_taskHandle) {
        _lastError = ESP_OK;
        portEXIT_CRITICAL(&_mux);
        return true;
    }
    _taskStopRequested = false;
    _producedCallbacks = 0;
    _consumedCallbacks = 0;
    _cancelThrough = 0;
    portEXIT_CRITICAL(&_mux);

    char taskName[16];
    snprintf(taskName, sizeof(taskName), "AyTimerSW_%s", _name);
    TaskHandle_t handle = nullptr;
    const BaseType_t core = _taskCore == AYRES_TIMER_AUTO_CORE
        ? xPortGetCoreID()
        : _taskCore;
    const BaseType_t created = xTaskCreatePinnedToCore(
        &AyresTimer<AYRES_SOFTWARE>::callbackTask_, taskName,
        _taskStackSize, this, _taskPriority, &handle, core);
    if (created != pdPASS || !handle) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }

    portENTER_CRITICAL(&_mux);
    _taskHandle = handle;
    _lastError = ESP_OK;
    portEXIT_CRITICAL(&_mux);
    return true;
}

void AyresTimer<AYRES_SOFTWARE>::stopTask_() {
    portENTER_CRITICAL(&_mux);
    TaskHandle_t handle = _taskHandle;
    _taskStopRequested = true;
    portEXIT_CRITICAL(&_mux);
    if (!handle) return;

    xTaskNotifyGive(handle);
    if (xTaskGetCurrentTaskHandle() == handle) return;
    for (;;) {
        portENTER_CRITICAL(&_mux);
        const bool stopped = (_taskHandle == nullptr);
        portEXIT_CRITICAL(&_mux);
        if (stopped) break;
        vTaskDelay(kTaskShutdownPollTicks);
    }
}

bool AyresTimer<AYRES_SOFTWARE>::useTask(bool enable, UBaseType_t priority,
                                         uint32_t stackSize, BaseType_t core) {
    if (xPortInIsrContext() || stackSize < 1024 ||
        priority >= configMAX_PRIORITIES || !validCore(core)) {
        setError_(ESP_ERR_INVALID_ARG);
        return false;
    }
    RecursiveGuard api(_apiMutex);
    if (!api) {
        setError_(ESP_ERR_NO_MEM);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    const bool busy = _running || _callbackActive ||
                      _producedCallbacks != _consumedCallbacks;
    portEXIT_CRITICAL(&_mux);
    if (busy) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    if (enable) {
        portENTER_CRITICAL(&_mux);
        const bool hasISRCallback = _isrCallback != nullptr;
        const bool taskExists = _taskHandle != nullptr;
        const bool changed = _taskPriority != priority ||
                             _taskStackSize != stackSize || _taskCore != core;
        portEXIT_CRITICAL(&_mux);
        if (hasISRCallback) {
            setError_(ESP_ERR_INVALID_STATE);
            return false;
        }
        if (taskExists && changed) stopTask_();
        portENTER_CRITICAL(&_mux);
        _useTask = true;
        _taskPriority = priority;
        _taskStackSize = stackSize;
        _taskCore = core;
        portEXIT_CRITICAL(&_mux);
        return ensureTask_();
    }

    portENTER_CRITICAL(&_mux);
    const bool dispatchInISR = _dispatchInISR;
    const bool hasCppCallback = _hasCppCallback;
    portEXIT_CRITICAL(&_mux);
    if (dispatchInISR && hasCppCallback) {
        setError_(ESP_ERR_INVALID_STATE);
        return false;
    }
    portENTER_CRITICAL(&_mux);
    _useTask = false;
    portEXIT_CRITICAL(&_mux);
    stopTask_();
    setError_(ESP_OK);
    return true;
}

bool IRAM_ATTR AyresTimer<AYRES_SOFTWARE>::dispatch_() {
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL_ISR(&_mux);
    if (_mode != PERIODIC) {
        _running = false;
    }

    // Telemetría
    ++_stats.totalTriggers;
    _stats.lastTriggerUs = nowUs;
    if (_expectedNextUs > 0) {
        const int64_t jitter = static_cast<int64_t>(nowUs) - static_cast<int64_t>(_expectedNextUs);
        _stats.lastJitterUs = jitter;
        const int64_t absJitter = jitter < 0 ? -jitter : jitter;
        if (absJitter > _stats.maxJitterUs) {
            _stats.maxJitterUs = absJitter;
        }
    }
    if (_mode == PERIODIC) {
        _expectedNextUs = nowUs + _timeout_us;
    } else {
        _expectedNextUs = 0;
    }

    const bool hasCpp = _hasCppCallback;
    const bool hasIsr = _isrCallback != nullptr;
    const bool useDedicatedTask = _useTask || (_dispatchInISR && hasCpp);
    TaskHandle_t task = useDedicatedTask ? _taskHandle : nullptr;
    ISRCallback isrCb = _isrCallback;
    void* isrArg = _isrArg;
    if (task) ++_producedCallbacks;
    portEXIT_CRITICAL_ISR(&_mux);

    if (task) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        return woken == pdTRUE;
    }

    if (hasIsr && isrCb) {
        isrCb(isrArg);
        return false;
    }

    if (hasCpp) {
        if (_callbackMutex) {
            xSemaphoreTakeRecursive(_callbackMutex, portMAX_DELAY);
            portENTER_CRITICAL_ISR(&_mux);
            const bool callCpp = _hasCppCallback;
            _callbackActive = callCpp;
            portEXIT_CRITICAL_ISR(&_mux);
            xSemaphoreGiveRecursive(_callbackMutex);
            if (callCpp && _callback) _callback();
            portENTER_CRITICAL_ISR(&_mux);
            _callbackActive = false;
            portEXIT_CRITICAL_ISR(&_mux);
        }
    }
    return false;
}

void IRAM_ATTR AyresTimer<AYRES_SOFTWARE>::trampoline(void* arg) {
    auto* self = static_cast<AyresTimer<AYRES_SOFTWARE>*>(arg);
    self->dispatch_();
}

void AyresTimer<AYRES_SOFTWARE>::callbackTask_(void* pv) {
    auto* self = static_cast<AyresTimer<AYRES_SOFTWARE>*>(pv);
    for (;;) {
        (void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        portENTER_CRITICAL(&self->_mux);
        const bool stopRequested = self->_taskStopRequested;
        const uint64_t sequence = ++self->_consumedCallbacks;
        const bool cancelled = sequence <= self->_cancelThrough;
        if (cancelled) ++self->_stats.droppedCallbacks;
        portEXIT_CRITICAL(&self->_mux);
        if (stopRequested) break;
        if (cancelled) continue;

        if (self->_callbackMutex) {
            xSemaphoreTakeRecursive(self->_callbackMutex, portMAX_DELAY);
            portENTER_CRITICAL(&self->_mux);
            const bool callCpp = self->_hasCppCallback;
            self->_callbackActive = callCpp;
            portEXIT_CRITICAL(&self->_mux);
            xSemaphoreGiveRecursive(self->_callbackMutex);
            if (callCpp && self->_callback) self->_callback();
            portENTER_CRITICAL(&self->_mux);
            self->_callbackActive = false;
            portEXIT_CRITICAL(&self->_mux);
        }
    }

    portENTER_CRITICAL(&self->_mux);
    self->_taskHandle = nullptr;
    portEXIT_CRITICAL(&self->_mux);
    vTaskDelete(nullptr);
}

uint32_t AyresTimer<AYRES_SOFTWARE>::cpuFreqMHz() {
    return getCpuFrequencyMhz();
}

uint32_t AyresTimer<AYRES_SOFTWARE>::cycles() {
    return xthal_get_ccount();
}

void AyresTimer<AYRES_SOFTWARE>::busyWaitCycles(uint32_t cyc) {
    const uint32_t start = xthal_get_ccount();
    while ((xthal_get_ccount() - start) < cyc) {
        __asm__ __volatile__("nop");
    }
}

void AyresTimer<AYRES_SOFTWARE>::busyWaitUs(uint32_t us) {
    const uint32_t start = xthal_get_ccount();
    const uint32_t cyc = us * getCpuFrequencyMhz();
    while ((xthal_get_ccount() - start) < cyc) {
        __asm__ __volatile__("nop");
    }
}

uint64_t AyresTimer<AYRES_SOFTWARE>::micros64() {
    return static_cast<uint64_t>(esp_timer_get_time());
}
