#pragma once

// Reads current values from 2 ADC pins on the BTS 7200 switched between with a selection pin.
// Requires GPIO, interrupts, soft timers, and ADC to be initialized in ADC_MODE_SINGLE.

#include "adc.h"
#include "gpio.h"
#include "soft_timer.h"
#include "mcp23008_gpio_expander.h"

typedef void (*Bts7200DataCallback)(uint16_t reading_out_0, uint16_t reading_out_1, void *context);

// Use when the select pin is a native GPIO pin
typedef struct {
  GpioAddress *select_pin;
  GpioAddress *sense_pin;
  uint32_t interval_us;
  Bts7200DataCallback callback;  // set to NULL for no callback
  void *callback_context;
} Bts7200NativeSettings;

// Use when the select pin is through an MCP23008 GPIO expander
typedef struct {
  Mcp23008GpioAddress *select_pin;
  GpioAddress *sense_pin;
  uint32_t interval_us;
  Bts7200DataCallback callback;  // set to NULL for no callback
  void *callback_context;
} Bts7200Mcp23008Settings;

typedef enum {
  BTS7200_SELECT_PIN_NATIVE = 0,
  BTS7200_SELECT_PIN_MCP23008,
  NUM_BTS7200_SELECT_PINS,
} Bts7200SelectPinType;

typedef struct {
  uint16_t reading_out_0;
  uint16_t reading_out_1;
  GpioAddress *select_pin_native;
  Mcp23008GpioAddress *select_pin_mcp23008;
  Bts7200SelectPinType select_pin_type;
  GpioAddress *sense_pin;
  uint32_t interval_us;
  SoftTimerId timer_id;
  Bts7200DataCallback callback;
  void *callback_context;
} Bts7200Storage;

// Initialize the BTS7200 with the given settings; the select pin is a native GPIO pin.
StatusCode bts_7200_init_native(Bts7200Storage *storage, Bts7200NativeSettings *settings);

// Initialize the BTS7200 with the given settings; the select pin is through an MCP23008.
StatusCode bts_7200_init_mcp23008(Bts7200Storage *storage, Bts7200Mcp23008Settings *settings);

// Read the latest measurements. This does not get measurements from the storage but instead
// reads them from the BTS7200 itself.
StatusCode bts_7200_get_measurement(Bts7200Storage *storage, uint16_t *meas0, uint16_t *meas1);

// Set up a soft timer which periodically updates the storage with the latest measurements.
StatusCode bts_7200_start(Bts7200Storage *storage);

// Stop the timer associated with the storage and return whether it was successful.
bool bts_7200_stop(Bts7200Storage *storage);
