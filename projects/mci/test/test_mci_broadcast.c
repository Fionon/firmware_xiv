#include "mci_broadcast.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ms_test_helpers.h"
#include "test_helpers.h"

#include "can.h"
#include "can_msg_defs.h"
#include "can_unpack.h"
#include "delay.h"
#include "event_queue.h"
#include "exported_enums.h"
#include "generic_can.h"
#include "generic_can_mcp2515.h"
#include "interrupt.h"
#include "log.h"
#include "mcp2515.h"
#include "soft_timer.h"
#include "status.h"

#include "mci_events.h"
#include "motor_can.h"
#include "motor_controller.h"
#include "wavesculptor.h"

#define TEST_CAN_DEVICE_ID 12

typedef enum {
  TEST_MCI_VELOCITY_MESSAGE = 0,
  TEST_MCI_BUS_MEASUREMENT_MESSAGE = 1,
  NUM_TEST_MCI_MESSAGES
} TestMciMessage;

static GenericCanMcp2515 s_motor_can;
static CanStorage s_can_storage;

static bool s_recieved_velocity = false;
static bool s_recieved_bus_measurement = false;

static MotorControllerBroadcastSettings s_broadcast_settings =
    { .motor_can = (GenericCan *)&s_motor_can,
      .device_ids = {
          [LEFT_MOTOR_CONTROLLER] = MOTOR_CAN_ID_LEFT_MOTOR_CONTROLLER,
          [RIGHT_MOTOR_CONTROLLER] = MOTOR_CAN_ID_RIGHT_MOTOR_CONTROLLER,
      } };

typedef struct TestWaveSculptorBusMeasurement {
  uint16_t bus_voltage_v;
  uint16_t bus_current_a;
} TestWaveSculptorBusMeasurement;

typedef struct TestMotorControllerMeasurements {
  TestWaveSculptorBusMeasurement bus_measurements[NUM_MOTOR_CONTROLLERS];
  uint16_t vehicle_velocity[NUM_MOTOR_CONTROLLERS];
} TestMotorControllerMeasurements;

static TestMotorControllerMeasurements s_test_measurements = { 0 };

static MotorCanFrameId s_frame_id_map[] = {
  [LEFT_MOTOR_CONTROLLER * NUM_TEST_MCI_MESSAGES + TEST_MCI_VELOCITY_MESSAGE] =
      MOTOR_CAN_LEFT_VELOCITY_MEASUREMENT_FRAME_ID,
  [RIGHT_MOTOR_CONTROLLER * NUM_TEST_MCI_MESSAGES + TEST_MCI_VELOCITY_MESSAGE] =
      MOTOR_CAN_RIGHT_VELOCITY_MEASUREMENT_FRAME_ID,
  [LEFT_MOTOR_CONTROLLER * NUM_TEST_MCI_MESSAGES + TEST_MCI_BUS_MEASUREMENT_MESSAGE] =
      MOTOR_CAN_LEFT_BUS_MEASUREMENT_FRAME_ID,
  [RIGHT_MOTOR_CONTROLLER * NUM_TEST_MCI_MESSAGES + TEST_MCI_BUS_MEASUREMENT_MESSAGE] =
      MOTOR_CAN_RIGHT_BUS_MEASUREMENT_FRAME_ID,
};

static StatusCode prv_handle_velocity(const CanMessage *msg, void *context,
                                      CanAckStatus *ack_reply) {
  uint16_t left_velocity, right_velocity;
  CAN_UNPACK_MOTOR_VELOCITY(msg, &left_velocity, &right_velocity);
  s_test_measurements.vehicle_velocity[LEFT_MOTOR_CONTROLLER] = left_velocity;
  s_test_measurements.vehicle_velocity[RIGHT_MOTOR_CONTROLLER] = right_velocity;
  s_recieved_velocity = true;
  return STATUS_CODE_OK;
}

static StatusCode prv_handle_bus_measurement(const CanMessage *msg, void *context,
                                             CanAckStatus *ack_reply) {
  uint16_t left_voltage, left_current, right_voltage, right_current;
  CAN_UNPACK_MOTOR_CONTROLLER_VC(msg, &left_voltage, &left_current, &right_voltage, &right_current);
  s_test_measurements.bus_measurements[LEFT_MOTOR_CONTROLLER].bus_voltage_v = left_voltage;
  s_test_measurements.bus_measurements[LEFT_MOTOR_CONTROLLER].bus_current_a = left_current;
  s_test_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_voltage_v = right_voltage;
  s_test_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_current_a = right_current;
  s_recieved_bus_measurement = true;
  return STATUS_CODE_OK;
}

static void prv_send_measurements(MotorController controller, TestMciMessage message_type,
                                  MotorControllerMeasurements *measurements) {
  WaveSculptorCanData can_data = { 0 };
  if (message_type == TEST_MCI_VELOCITY_MESSAGE) {
    can_data.velocity_measurement.motor_velocity_rpm = 0;
    can_data.velocity_measurement.vehicle_velocity_ms = measurements->vehicle_velocity[controller];
  } else if (message_type == TEST_MCI_BUS_MEASUREMENT_MESSAGE) {
    can_data.bus_measurement.bus_voltage_v =
        measurements->bus_measurements[controller].bus_voltage_v;
    can_data.bus_measurement.bus_current_a =
        measurements->bus_measurements[controller].bus_current_a;
  }

  GenericCanMsg msg = {
    .id = s_frame_id_map[controller * NUM_TEST_MCI_MESSAGES + message_type],
    .dlc = sizeof(can_data),
    .extended = false,
  };
  memcpy(&msg.data, &can_data, sizeof(can_data));

  generic_can_tx((GenericCan *)&s_motor_can, &msg);
}

StatusCode TEST_MOCK(mcp2515_tx)(Mcp2515Storage *storage, uint32_t id, bool extended, uint64_t data,
                                 size_t dlc) {
  if (storage->rx_cb != NULL) {
    storage->rx_cb(id, extended, data, dlc, storage->context);
  }
  return STATUS_CODE_OK;
}

static void prv_rx_handler(uint32_t id, bool extended, uint64_t data, size_t dlc, void *context) {
  GenericCanMcp2515 *gcmcp = context;
  for (size_t i = 0; i < NUM_GENERIC_CAN_RX_HANDLERS; i++) {
    if (gcmcp->base.rx_storage[i].rx_handler != NULL &&
        (id & gcmcp->base.rx_storage[i].mask) == gcmcp->base.rx_storage[i].filter) {
      const GenericCanMsg msg = {
        .id = id,
        .extended = extended,
        .data = data,
        .dlc = dlc,
      };
      gcmcp->base.rx_storage[i].rx_handler(&msg, gcmcp->base.rx_storage[i].context);
      break;
    }
  }
}

static void prv_setup_motor_can(void) {
  Mcp2515Settings mcp2515_settings = {
    .spi_port = SPI_PORT_2,
    .spi_baudrate = 6000000,
    .mosi = { .port = GPIO_PORT_B, 15 },
    .miso = { .port = GPIO_PORT_B, 14 },
    .sclk = { .port = GPIO_PORT_B, 13 },
    .cs = { .port = GPIO_PORT_B, 12 },
    .int_pin = { .port = GPIO_PORT_A, 8 },

    .can_bitrate = MCP2515_BITRATE_500KBPS,
    .loopback = true,
  };

  // Will throw unimplemented errors on x86
  generic_can_mcp2515_init(&s_motor_can, &mcp2515_settings);
  // On x86 the rx handler doesn't get registered either :|
  mcp2515_register_cbs(s_motor_can.mcp2515, prv_rx_handler, NULL, &s_motor_can);
}

static void prv_setup_system_can() {
  CanSettings can_settings = {
    .device_id = SYSTEM_CAN_DEVICE_MOTOR_CONTROLLER,
    .bitrate = CAN_HW_BITRATE_500KBPS,
    .rx_event = MCI_CAN_EVENT_RX,
    .tx_event = MCI_CAN_EVENT_TX,
    .fault_event = MCI_CAN_EVENT_FAULT,
    .tx = { GPIO_PORT_A, 12 },
    .rx = { GPIO_PORT_A, 11 },
    .loopback = true,
  };

  can_init(&s_can_storage, &can_settings);
}

static void prv_assert_double_broadcast() {
  MS_TEST_HELPER_CAN_TX(MCI_CAN_EVENT_TX);
  MS_TEST_HELPER_CAN_TX(MCI_CAN_EVENT_TX);
  MS_TEST_HELPER_CAN_RX(MCI_CAN_EVENT_RX);
  MS_TEST_HELPER_CAN_RX(MCI_CAN_EVENT_RX);
  MS_TEST_HELPER_ASSERT_NO_EVENT_RAISED();
}

static void prv_assert_single_broadcast() {
  MS_TEST_HELPER_CAN_TX_RX(MCI_CAN_EVENT_TX, MCI_CAN_EVENT_RX);
  MS_TEST_HELPER_ASSERT_NO_EVENT_RAISED();
}

void setup_test(void) {
  event_queue_init();
  interrupt_init();
  soft_timer_init();

  prv_setup_system_can();
  prv_setup_motor_can();

  TEST_ASSERT_OK(can_register_rx_handler(SYSTEM_CAN_MESSAGE_MOTOR_CONTROLLER_VC,
                                         prv_handle_bus_measurement, NULL));
  TEST_ASSERT_OK(
      can_register_rx_handler(SYSTEM_CAN_MESSAGE_MOTOR_VELOCITY, prv_handle_velocity, NULL));
}

void teardown_test(void) {
  s_recieved_velocity = false;
  s_recieved_bus_measurement = false;
  memset(&s_test_measurements, 0, sizeof(s_test_measurements));
}

// lv - left velocity
// rv - right velocity
// lb - left bus measurement
// rb - right bus measurement

// Test 1: lb rv lv rb (check 2 output)
void test_all_measurements_lb_rv_lv_rb() {
  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);
  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 12.345,
                    .bus_current_a = 5.9876,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 56.5665,
        },
  };

  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_double_broadcast();

  TEST_ASSERT_TRUE(s_recieved_velocity);
  TEST_ASSERT_TRUE(s_recieved_bus_measurement);
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 2: lb rb lv rv (check 2 output)
void test_all_measurements_lb_rb_lv_rv() {
  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 12.345,
                    .bus_current_a = 5.9876,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 56.5665,
        },
  };

  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_double_broadcast();

  TEST_ASSERT_TRUE(s_recieved_velocity);
  TEST_ASSERT_TRUE(s_recieved_bus_measurement);
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 3: rb lb lv rv (check 2 output)
void test_all_measurements_rb_lb_lv_rv() {
  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 12.345,
                    .bus_current_a = 5.9876,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 56.5665,
        },
  };

  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_double_broadcast();

  TEST_ASSERT_TRUE(s_recieved_velocity);
  TEST_ASSERT_TRUE(s_recieved_bus_measurement);
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 4: rb (check no output)
void test_no_measurements_rb() {
  // motor_can tx and rx should happen immedietly

  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 0,
            [RIGHT_MOTOR_CONTROLLER] = 0,
        },
  };

  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  MS_TEST_HELPER_ASSERT_NO_EVENT_RAISED();

  TEST_ASSERT_FALSE(s_recieved_velocity);
  TEST_ASSERT_FALSE(s_recieved_bus_measurement);

  expected_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_voltage_v = 0;
  expected_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_current_a = 0;
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 4: rb lb (check only 1 output)
void test_bus_measurements_rb_lb() {
  // motor_can tx and rx should happen immedietly

  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 12.345,
                    .bus_current_a = 5.9876,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 0,
            [RIGHT_MOTOR_CONTROLLER] = 0,
        },
  };

  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_single_broadcast();

  TEST_ASSERT_FALSE(s_recieved_velocity);
  TEST_ASSERT_TRUE(s_recieved_bus_measurement);
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 5: rv lv (check only 1 output)
void test_velocity_measurements_rv_lv() {
  // motor_can tx and rx should happen immedietly

  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 56.5665,
        },
  };

  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_single_broadcast();

  TEST_ASSERT_TRUE(s_recieved_velocity);
  TEST_ASSERT_FALSE(s_recieved_bus_measurement);
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 6: rb lv (check no output)
void test_no_measurements_rb_lv() {
  // motor_can tx and rx should happen immedietly

  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 0,
        },
  };

  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE, &expected_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  MS_TEST_HELPER_ASSERT_NO_EVENT_RAISED();

  TEST_ASSERT_FALSE(s_recieved_velocity);
  TEST_ASSERT_FALSE(s_recieved_bus_measurement);

  expected_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_voltage_v = 0;
  expected_measurements.bus_measurements[RIGHT_MOTOR_CONTROLLER].bus_current_a = 0;
  expected_measurements.vehicle_velocity[LEFT_MOTOR_CONTROLLER] = 0;
  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_voltage_v,
                      s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL((uint16_t)expected_measurements.bus_measurements[motor_id].bus_current_a,
                      s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}

// Test 5: rv lv (check only 1 output) rb lb (check only 1 output)
void test_all_measurements_rv_lv_then_rb_lb() {
  // motor_can tx and rx should happen immedietly

  MotorControllerBroadcastStorage broadcast_storage = { 0 };
  mci_broadcast_init(&broadcast_storage, &s_broadcast_settings);

  MotorControllerMeasurements expected_first_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 0,
                    .bus_current_a = 0,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 1.0101,
            [RIGHT_MOTOR_CONTROLLER] = 56.5665,
        },
  };

  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE,
                        &expected_first_measurements);
  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_VELOCITY_MESSAGE,
                        &expected_first_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_single_broadcast();

  TEST_ASSERT_TRUE(s_recieved_velocity);
  TEST_ASSERT_FALSE(s_recieved_bus_measurement);

  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL(
        (uint16_t)expected_first_measurements.bus_measurements[motor_id].bus_voltage_v,
        s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL(
        (uint16_t)expected_first_measurements.bus_measurements[motor_id].bus_current_a,
        s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_first_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }

  s_recieved_velocity = false;
  s_recieved_bus_measurement = false;
  memset(&s_test_measurements, 0, sizeof(s_test_measurements));

  MotorControllerMeasurements expected_second_measurements = {
    .bus_measurements =
        {
            [LEFT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 12.345,
                    .bus_current_a = 5.9876,
                },
            [RIGHT_MOTOR_CONTROLLER] =
                {
                    .bus_voltage_v = 4.8602,
                    .bus_current_a = 1.3975,
                },
        },
    .vehicle_velocity =
        {
            [LEFT_MOTOR_CONTROLLER] = 0,
            [RIGHT_MOTOR_CONTROLLER] = 0,
        },
  };

  prv_send_measurements(LEFT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_second_measurements);
  prv_send_measurements(RIGHT_MOTOR_CONTROLLER, TEST_MCI_BUS_MEASUREMENT_MESSAGE,
                        &expected_second_measurements);

  delay_ms(MOTOR_CONTROLLER_BROADCAST_TX_PERIOD_MS + 50);
  prv_assert_single_broadcast();

  TEST_ASSERT_FALSE(s_recieved_velocity);
  TEST_ASSERT_TRUE(s_recieved_bus_measurement);

  for (size_t motor_id = 0; motor_id < NUM_MOTOR_CONTROLLERS; motor_id++) {
    TEST_ASSERT_EQUAL(
        (uint16_t)expected_second_measurements.bus_measurements[motor_id].bus_voltage_v,
        s_test_measurements.bus_measurements[motor_id].bus_voltage_v);
    TEST_ASSERT_EQUAL(
        (uint16_t)expected_second_measurements.bus_measurements[motor_id].bus_current_a,
        s_test_measurements.bus_measurements[motor_id].bus_current_a);
    TEST_ASSERT_EQUAL((uint16_t)(expected_second_measurements.vehicle_velocity[motor_id] * 100),
                      s_test_measurements.vehicle_velocity[motor_id]);
  }
}
