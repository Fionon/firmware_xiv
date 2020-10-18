#include "interrupt.h"
#include "log.h"
#include "soft_timer.h"
#include "wait.h"

#define COUNTER_PERIOD_MS 500

typedef struct Counters {
  uint8_t counter_a;
  uint8_t counter_b;
} Counters;

static void prv_timer_callback(SoftTimerId timer_id, void *context) {
  Counters *storage = context;
  storage->counter_a++;

  LOG_DEBUG("Counter A: %d\n", storage->counter_a);

  if (storage->counter_a == storage->counter_b * 2 + 2) {
    storage->counter_b++;
    LOG_DEBUG("Counter B: %d\n", storage->counter_b);
  }

  soft_timer_start_millis(COUNTER_PERIOD_MS, prv_timer_callback, storage, NULL);
}

int main(void) {
  interrupt_init();
  soft_timer_init();

  Counters storage = { 0 };

  soft_timer_start_millis(COUNTER_PERIOD_MS, prv_timer_callback, &storage, NULL);

  while (true) {
    wait();
  }

  return 0;
}