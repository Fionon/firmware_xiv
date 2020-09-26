#pragma once

typedef enum {
  // Digital
  STEERING_INPUT_HORN_EVENT = 0,
  STEERING_RADIO_PPT_EVENT,
  STEERING_HIGH_BEAM_FORWARD_EVENT,
  STEERING_HIGH_BEAM_REAR_EVENT,
  STEERING_REGEN_BRAKE_EVENT,
  STEERING_DIGITAL_INPUT_CC_TOGGLE_PRESSED_EVENT,
  STEERING_CC_INCREASE_SPEED_EVENT,
  STEERING_CC_DECREASE_SPEED_EVENT,
} SteeringDigitalEvent;

typedef enum {
  // Analog
  STEERING_CONTROL_STALK_EVENT_LEFT_SIGNAL = STEERING_CC_DECREASE_SPEED_EVENT + 1,
  STEERING_CONTROL_STALK_EVENT_RIGHT_SIGNAL,
  STEERING_CONTROL_STALK_EVENT_NO_SIGNAL,
} SteeringAnalogEvent;

typedef enum {
  STEERING_CAN_EVENT_RX = STEERING_CONTROL_STALK_EVENT_NO_SIGNAL + 1,
  STEERING_CAN_EVENT_TX,
  STEERING_CAN_FAULT,
  NUM_TOTAL_STEERING_EVENTS,
} SteeringCanEvent;
