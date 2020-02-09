#include "mci_output.h"

#include <string.h>

#include "motor_can.h"
#include "motor_controller.h"
#include "wavesculptor.h"

#include "exported_enums.h"
#include "generic_can.h"
#include "pedal_rx.h"
#include "soft_timer.h"
#include "status.h"

static float s_velocity_lookup[] = {
  [EE_DRIVE_OUTPUT_OFF] = 0.0f,
  [EE_DRIVE_OUTPUT_DRIVE] = WAVESCULPTOR_FORWARD_VELOCITY,
  [EE_DRIVE_OUTPUT_REVERSE] = WAVESCULPTOR_REVERSE_VELOCITY,
};

static float s_max_regen_in_throttle = 0.0f;
static float s_regen_threshold = 0.0f;

// Basic implementation of throttle/brake maps
static float prv_brake_to_regen_map(float brake_value) {
  return brake_value / PEDAL_RX_MAX_PEDAL_VALUE;
}

static float prv_throttle_to_regen_map(float throttle_value) {
  return 0.0f;
}

static float prv_throttle_to_accel_map(float throttle_value) {
  return throttle_value / PEDAL_RX_MAX_PEDAL_VALUE;
}

static void prv_send_wavesculptor_message(MotorControllerStorage *storage,
                                          MotorControllerId motor_controlller_id,
                                          MotorCanDriveCommand command) {
  GenericCanMsg msg = {
    .id = motor_controlller_id,
    .dlc = MOTOR_CAN_DRIVE_COMMAND_LENGTH,
    .extended = MOTOR_CAN_DRIVE_COMMAND_IS_EXTENDED,
  };

  uint8_t data[MOTOR_CAN_DRIVE_COMMAND_LENGTH] = { 0 };
  motor_can_drive_command_pack(data, &command, sizeof(data));
  memcpy(&msg.data, data, sizeof(data));
  generic_can_tx(storage->motor_can, &msg);
}

static void prv_handle_drive(SoftTimerId timer_id, void *context) {
  MotorControllerStorage *storage = context;
  PedalValues pedal_values = pedal_rx_get_pedal_values(&storage->pedal_storage);
  MotorCanDriveCommand drive_command = { 0 };
  // TODO(SOFT-122): Make sure test ensures that maps are continues
  if (storage->drive_state == EE_DRIVE_OUTPUT_OFF) {
    drive_command.motor_current = 0.0f;
    drive_command.motor_velocity = 0.0f;
  } else if (pedal_values.brake > MOTOR_CONTROLLER_BRAKE_THRESHOLD) {
    // Regen Braking along with brake being pressed
    drive_command.motor_current = prv_brake_to_regen_map(pedal_values.brake);
    drive_command.motor_velocity = 0.0f;
  } else if (pedal_values.throttle < s_regen_threshold) {
    // Regen Braking along if throttle is pressed a little
    drive_command.motor_current = prv_throttle_to_regen_map(pedal_values.throttle);
    drive_command.motor_velocity = 0.0f;
  } else {
    drive_command.motor_current = prv_throttle_to_accel_map(pedal_values.throttle);
    drive_command.motor_velocity = s_velocity_lookup[storage->drive_state];
  }
  /** Handling message **/
  prv_send_wavesculptor_message(storage, MOTOR_CAN_LEFT_DRIVE_COMMAND_FRAME_ID, drive_command);
  prv_send_wavesculptor_message(storage, MOTOR_CAN_RIGHT_DRIVE_COMMAND_FRAME_ID, drive_command);
  soft_timer_start_millis(MOTOR_CONTROLLER_DRIVE_TX_PERIOD_MS, prv_handle_drive, storage, NULL);
}

StatusCode mci_output_init(MotorControllerStorage *storage) {
  return soft_timer_start_millis(MOTOR_CONTROLLER_DRIVE_TX_PERIOD_MS, prv_handle_drive, storage,
                                 NULL);
}
