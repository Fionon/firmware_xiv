#include "mcp3427_adc.h"
#include "fsm.h"
#include "log.h"
#include "mcp3427_adc_defs.h"
#include "soft_timer.h"

// Note: we always read from both channels before reporting, taking 100ms between reports. If timing
// is an issue, an optimization for boards (i.e. solar) using only one channel is to only read that
// channel, so reports take 50ms.

// We use a combination of the address and I2C port in the event data to only transition on events
// that we raised. Possible optimization if necessary: make prv_get_chip_identifier public, then
// keep a reverse lookup table of identifier -> MCP3427 storage and only call the appropriate
// MCP3427 when receiving an MCP3427 event. That way, using n MCP3427s generates n calls to
// |mcp3427_process_event| per cycle rather than n^2 if we pass every event to every MCP3427.
// (I think this was done on MSXII.)

#define MCP3427_FSM_NAME "MCP3427 FSM"

FSM_DECLARE_STATE(channel_1_trigger);
FSM_DECLARE_STATE(channel_1_readback);
FSM_DECLARE_STATE(channel_2_trigger);
FSM_DECLARE_STATE(channel_2_readback);

FSM_STATE_TRANSITION(channel_1_trigger) {
  Mcp3427Storage *storage = fsm->context;
  FSM_ADD_TRANSITION(storage->data_ready_event, channel_1_readback);
}

FSM_STATE_TRANSITION(channel_1_readback) {
  Mcp3427Storage *storage = fsm->context;
  FSM_ADD_TRANSITION(storage->data_trigger_event, channel_2_trigger);
}

FSM_STATE_TRANSITION(channel_2_trigger) {
  Mcp3427Storage *storage = fsm->context;
  FSM_ADD_TRANSITION(storage->data_ready_event, channel_2_readback);
}

FSM_STATE_TRANSITION(channel_2_readback) {
  Mcp3427Storage *storage = fsm->context;
  FSM_ADD_TRANSITION(storage->data_trigger_event, channel_1_trigger);
}

static uint16_t prv_get_chip_identifier(Mcp3427Storage *storage) {
  // used to gate events we raised to only this MCP3427
  return (storage->addr << 8) | (storage->port == I2C_PORT_1 ? 0 : 1);
}

static void prv_raise_ready(SoftTimerId timer_id, void *context) {
  Mcp3427Storage *storage = (Mcp3427Storage *)context;
  event_raise(storage->data_ready_event, prv_get_chip_identifier(storage));
}

static uint16_t s_data_mask_lookup[] = {
  [MCP3427_SAMPLE_RATE_12_BIT] = MCP3427_DATA_MASK_12_BIT,  //
  [MCP3427_SAMPLE_RATE_14_BIT] = MCP3427_DATA_MASK_14_BIT,  //
  [MCP3427_SAMPLE_RATE_16_BIT] = MCP3427_DATA_MASK_16_BIT,  //
};

static void prv_channel_ready(struct Fsm *fsm, const Event *e, void *context) {
  Mcp3427Storage *storage = (Mcp3427Storage *)context;
  uint8_t read_data[MCP3427_NUM_DATA_BYTES] = { 0 };
  StatusCode status = i2c_read(storage->port, storage->addr, read_data, MCP3427_NUM_DATA_BYTES);
  // The third byte is the config/status byte. It contains the ready bit.
  uint8_t config = read_data[2];
  // If the latest data is not ready, we log it.

  if (config & MCP3427_RDY_MASK) {
    LOG_WARN("MCP3427 ADC: Ready bit not cleared. Data may not be the latest data available.\n");
    if (storage->fault_callback != NULL) {
      storage->fault_callback(storage->fault_context);
    }
    event_raise(storage->data_trigger_event, prv_get_chip_identifier(storage));
    return;
  }

  // The first and second bytes contain latest ADC value.
  // The appropriate number of bits is taken, depending on the sample rate.
  uint16_t sensor_data = (read_data[0] << 8) | read_data[1];
  sensor_data &= s_data_mask_lookup[storage->sample_rate];

  Mcp3427Channel current_channel =
      (storage->config & (1 << MCP3427_CH_SEL_OFFSET)) >> MCP3427_CH_SEL_OFFSET;

  storage->sensor_data[current_channel] = sensor_data;

  if (current_channel == MCP3427_CHANNEL_2 && storage->callback != NULL) {
    // We have all the data ready.
    storage->callback((int16_t)storage->sensor_data[0], (int16_t)storage->sensor_data[1],
                      storage->context);
  }

  event_raise(storage->data_trigger_event, prv_get_chip_identifier(storage));
}

// Trigger data read. Schedule a data ready event to be raised.
static void prv_channel_trigger(struct Fsm *fsm, const Event *e, void *context) {
  Mcp3427Storage *storage = (Mcp3427Storage *)context;
  // We want to trigger a read. So we set the ready bit.
  // If operating in continuous conversion mode, setting this bit has no effect.
  storage->config |= MCP3427_RDY_MASK;
  // Setting the current channel we want to read from. We just flip it from the previous read.
  storage->config ^= (1 << MCP3427_CH_SEL_OFFSET);

  i2c_write(storage->port, storage->addr, &storage->config, MCP3427_NUM_CONFIG_BYTES);
  soft_timer_start_millis(MCP3427_MAX_CONV_TIME_MS, prv_raise_ready, storage, NULL);
}

// Lookup table for selected address. See manual table 5-3.
static uint8_t s_addr_lookup[NUM_MCP3427_PIN_STATES][NUM_MCP3427_PIN_STATES] = {
  { 0x0, 0x1, 0x2 },
  { 0x3, 0x0, 0x7 },
  { 0x4, 0x5, 0x6 },
};

StatusCode mcp3427_init(Mcp3427Storage *storage, Mcp3427Settings *settings) {
  if (storage == NULL || settings == NULL) {
    return status_code(STATUS_CODE_INVALID_ARGS);
  }

  storage->data_ready_event = settings->adc_data_ready_event;
  storage->data_trigger_event = settings->adc_data_trigger_event;

  fsm_state_init(channel_1_trigger, prv_channel_trigger);
  fsm_state_init(channel_1_readback, prv_channel_ready);
  fsm_state_init(channel_2_trigger, prv_channel_trigger);
  fsm_state_init(channel_2_readback, prv_channel_ready);
  storage->port = settings->port;
  storage->addr =
      s_addr_lookup[settings->addr_pin_0][settings->addr_pin_1] | (MCP3427_DEVICE_CODE << 3);

  // Writing configuration to the chip (see section 5.3.3 of manual).
  // Note: Here, channel gets defaulted to 0.
  uint8_t config = 0;
  config |= (settings->conversion_mode << MCP3427_CONVERSION_MODE_OFFSET);
  config |= (settings->sample_rate << MCP3427_SAMPLE_RATE_OFFSET);
  config |= (settings->amplifier_gain << MCP3427_GAIN_SEL_OFFSET);
  storage->config = config;

  StatusCode status = i2c_write(storage->port, storage->addr, &config, MCP3427_NUM_CONFIG_BYTES);
  // start the state machine ready to transition to channel_1_trigger
  fsm_init(&storage->fsm, MCP3427_FSM_NAME, &channel_2_readback, storage);
  return status;
}

StatusCode mcp3427_register_callback(Mcp3427Storage *storage, Mcp3427Callback callback,
                                     void *context) {
  if (storage == NULL || callback == NULL) {
    return status_code(STATUS_CODE_INVALID_ARGS);
  }
  storage->callback = callback;
  storage->context = context;
  return STATUS_CODE_OK;
}

StatusCode mcp3427_register_fault_callback(Mcp3427Storage *storage, Mcp3427FaultCallback callback,
                                           void *context) {
  if (storage == NULL || callback == NULL) {
    return status_code(STATUS_CODE_INVALID_ARGS);
  }
  storage->fault_callback = callback;
  storage->fault_context = context;
  return STATUS_CODE_OK;
}

StatusCode mcp3427_start(Mcp3427Storage *storage) {
  return event_raise(storage->data_trigger_event, prv_get_chip_identifier(storage));
}

StatusCode mcp3427_process_event(Mcp3427Storage *storage, Event *e) {
  if (storage == NULL) {
    return status_code(STATUS_CODE_INVALID_ARGS);
  }
  if (e->data == prv_get_chip_identifier(storage)) {
    // only process events raised by us
    fsm_process_event(&storage->fsm, e);
  }
  return STATUS_CODE_OK;
}
