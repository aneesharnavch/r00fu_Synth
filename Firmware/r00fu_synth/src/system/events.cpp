/*
 * events.cpp — the global event bus (implements events.h).
 *
 * One fixed-size FreeRTOS queue carries every cross-module Event. Hardware
 * scanners (matrix/mux) and the MIDI-IN drivers PRODUCE Events; the dispatcher
 * (Task_UI -> ui_dispatch) CONSUMES them via event_get(). This is the ONLY
 * sanctioned channel between modules (HARD RULE), so the whole API lives here:
 *
 *   events_init()          create the queue once at boot (before any post/get).
 *   event_post()           task-context enqueue (non-blocking, drop+count full).
 *   event_post_from_isr()  ISR-context enqueue (we have no ISR producers today,
 *                          but the contract requires it; kept correct + ready).
 *   event_get()            consumer blocks up to timeout_ms for the next Event.
 *   event_dropped_count()  diagnostics: Events lost to a full queue since boot.
 *
 * Sizing: EVENT_QUEUE_LEN must absorb a worst-case burst — a full 8x8 matrix
 * rescan plus a mux pass plus sequencer step fan-out — between two Task_UI
 * wake-ups. 64 entries is comfortably above that and keeps RAM modest (each
 * Event is 8 bytes, so the queue storage is ~512 B). Producers never block:
 * a 0-tick send drops + counts rather than stalling CORE_IO (matches the
 * non-blocking philosophy of the synth command queue).
 *
 * No dynamic allocation after init; no locks beyond the queue's own primitive.
 */
#include "events.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Depth of the global event ring. See sizing note above.
#define EVENT_QUEUE_LEN 64

// ── Module state ────────────────────────────────────────────────────────────
// Single static handle; created once in events_init(). Every API call guards on
// it being non-null so a mis-ordered boot fails safe (drops) instead of crashing.
static QueueHandle_t s_event_q = nullptr;

// Count of Events that could not be enqueued because the queue was full. Bumped
// from both task and ISR contexts; a plain volatile uint32_t is adequate for a
// monotonically-increasing diagnostic counter (we never need atomic read-modify
// correctness across the two producers, only a best-effort tally).
static volatile uint32_t s_dropped = 0;

// ── API ─────────────────────────────────────────────────────────────────────
bool events_init() {
  if (s_event_q != nullptr) return true;   // idempotent: safe to call twice
  s_event_q = xQueueCreate(EVENT_QUEUE_LEN, sizeof(Event));
  s_dropped = 0;
  return s_event_q != nullptr;
}

bool event_post(const Event& e) {
  if (s_event_q == nullptr) return false;
  // 0-tick timeout: never block the producing CORE_IO task. On a full queue we
  // drop the Event and count it rather than stalling a scanner/MIDI loop.
  if (xQueueSend(s_event_q, &e, 0) != pdTRUE) {
    s_dropped++;
    return false;
  }
  return true;
}

bool event_post_from_isr(const Event& e) {
  if (s_event_q == nullptr) return false;
  BaseType_t hpw = pdFALSE;   // set true if a higher-prio task was unblocked
  BaseType_t ok = xQueueSendFromISR(s_event_q, &e, &hpw);
  if (ok != pdTRUE) {
    s_dropped++;
    return false;
  }
  // Yield at the end of the ISR if posting woke a higher-priority consumer, so
  // the Event is serviced promptly instead of waiting for the next tick.
  portYIELD_FROM_ISR(hpw);
  return true;
}

bool event_get(Event& out, uint32_t timeout_ms) {
  if (s_event_q == nullptr) return false;
  // Block the consumer (Task_UI) up to timeout_ms for the next Event. A timeout
  // of 0 polls; the dispatcher passes a real timeout so it parks the core when
  // idle instead of spinning.
  const TickType_t ticks = (timeout_ms == 0)
                             ? 0
                             : pdMS_TO_TICKS(timeout_ms);
  return xQueueReceive(s_event_q, &out, ticks) == pdTRUE;
}

uint32_t event_dropped_count() {
  return s_dropped;
}
