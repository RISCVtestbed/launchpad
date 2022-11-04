#ifndef LAUNCHPAD_COMMON_H_
#define LAUNCHPAD_COMMON_H_

#include <stdint.h>

#define LP_STATUS_CODE unsigned int

#define LP_SUCCESS 0
#define LP_ERROR 1
#define LP_NOT_INITIALISED 2
#define LP_ALREADY_RUNNING 3
#define LP_ALREADY_STOPPED 4
#define LP_NOT_IMPLEMENTED 5
#define LP_UNKNOWN_CORE 6

enum LP_DEVICE_ARCHITECTURE_TYPE {LP_ARCH_TYPE_SHARED_NOTHING, LP_ARCH_TYPE_SHARED_INSTR_ONLY, LP_ARCH_TYPE_SHARED_DATA_ONLY, LP_ARCH_TYPE_SHARED_EVERYTHING};
enum LP_HOST_BOARD_TYPE {LP_PA100, LP_PA101, LP_BOARD_UNKNOWN};

struct host_board_status {
  float temp, power_draw;
  uint64_t time_alive_sec, num_power_cycles;
  int board_serial_number;
  enum LP_HOST_BOARD_TYPE board_type;
};

struct device_configuration {
  char * device_name, *cpu_name;
  int number_cores, clock_frequency_mhz, pcie_bar_ctrl_window_index, revision;
  char version;
  int * ddr_bank_mapping;
  uint64_t * ddr_base_addr_mapping;
  unsigned int instruction_space_size_mb, per_core_data_space_mb, shared_data_space_kb;
  enum LP_DEVICE_ARCHITECTURE_TYPE architecture_type;
};

struct device_drivers {
  LP_STATUS_CODE (*device_initialise)();
  LP_STATUS_CODE (*device_finalise)();
  LP_STATUS_CODE (*device_reset)();
  LP_STATUS_CODE (*device_get_configuration)(struct device_configuration*);
  LP_STATUS_CODE (*device_get_host_board_status)(struct host_board_status*);

  LP_STATUS_CODE (*device_start_core)(int);
  LP_STATUS_CODE (*device_start_allcores)();
  LP_STATUS_CODE (*device_stop_core)(int);
  LP_STATUS_CODE (*device_stop_allcores)();

  LP_STATUS_CODE (*device_write_instructions)(uint64_t, const char*, uint64_t);
  LP_STATUS_CODE (*device_write_data)(uint64_t, const char*, uint64_t);
  LP_STATUS_CODE (*device_read_data)(uint64_t, char*, uint64_t);

  LP_STATUS_CODE (*device_write_core_instructions)(int, uint64_t, const char*, uint64_t);
  LP_STATUS_CODE (*device_write_core_data)(int, uint64_t, const char*, uint64_t);
  LP_STATUS_CODE (*device_read_core_data)(int, uint64_t, char*, uint64_t);

  LP_STATUS_CODE (*device_read_gpio)(int, int, char*);
  LP_STATUS_CODE (*device_write_gpio)(int, int, char);
  LP_STATUS_CODE (*device_uart_has_data)(int, int*);
  LP_STATUS_CODE (*device_read_uart)(int, char*);
  LP_STATUS_CODE (*device_write_uart)(int, char);
  LP_STATUS_CODE (*device_raise_interrupt)(int, int);
};

#endif
