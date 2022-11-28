#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "launchpad_common.h"
#include "configuration.h"
#include "uart_interactive.h"
#include "util.h"

#ifdef MINOTAUR_SUPPORT
#include "minotaur.h"
#endif

static void display_device_configuration(struct device_configuration*, struct device_drivers*);
static void process_loop(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*);
static void check_number_cores_on_device_and_active(struct launchpad_configuration*, struct device_configuration*);
static char* parse_seconds_to_days(uint64_t, char*);

int main(int argc, char * argv[]) {
  struct launchpad_configuration * config=readConfiguration(argc, argv);
  struct device_drivers active_device_drivers;
  struct device_configuration device_config;
  struct current_device_status device_status;

#ifdef MINOTAUR_SUPPORT
  active_device_drivers=setup_minotaur_device_drivers();
#endif

  if (config->reset) check_device_status(active_device_drivers.device_reset());

  if (config->executable_filename != NULL || config->display_config) {
    check_device_status(active_device_drivers.device_initialise());
    device_status.initialised=true;
    check_device_status(active_device_drivers.device_get_configuration(&device_config));
    device_status.cores_active=(bool*) malloc(sizeof(bool) * device_config.number_cores);
    if (config->display_config) display_device_configuration(&device_config, &active_device_drivers);
    if (config->executable_filename != NULL) {
      check_number_cores_on_device_and_active(config, &device_config);
      transfer_executable_to_device(config, &device_config, &active_device_drivers);
      start_cores(config, &device_config, &active_device_drivers, &device_status);
      process_loop(config, &device_config, &active_device_drivers, &device_status);
    }
  }
  return 0;
}

static void display_device_configuration(struct device_configuration* device_config, struct device_drivers * active_device_drivers) {
  struct host_board_status board_status;
  check_device_status(active_device_drivers->device_get_host_board_status(&board_status));

  printf("Device: '%s', version %x revision %d\n", device_config->device_name, device_config->version, device_config->revision);
  printf("CPU configuration: %d cores of %s\n", device_config->number_cores, device_config->cpu_name);
  if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_NOTHING) {
    printf("Architecture type: Split instruction memory, split data memory\n");
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_INSTR_ONLY) {
    printf("Architecture type: Shared instruction memory, split data memory\n");
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_DATA_ONLY) {
    printf("Architecture type: Split instruction memory, shared data memory\n");
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_EVERYTHING) {
    printf("Architecture type: Shared instruction memory, shared data memory\n");
  }
  printf("Clock frequency: %dMHz\n", device_config->clock_frequency_mhz);
  printf("PCIe control BAR window: %d\n", device_config->pcie_bar_ctrl_window_index);
  printf("Memory configuration: %dMB instruction, %dMB data per core, %dKB shared data\n", device_config->instruction_space_size_mb,
    device_config->per_core_data_space_mb, device_config->shared_data_space_kb);

  bool ddr_inuse[2]={false, false};
  for (int i=0;i<device_config->number_cores;i++) {
    ddr_inuse[device_config->ddr_bank_mapping[i]]=true;
  }

  printf("\nDDR bank 0 in use: %s, DDR bank 1 in use: %s\n", ddr_inuse[0] ? "yes" : "no", ddr_inuse[1] ? "yes" : "no");

  for (int i=0;i<device_config->number_cores;i++) {
    printf("Core %d: DDR bank %d, host-side base data address 0x%lx\n", i, device_config->ddr_bank_mapping[i], device_config->ddr_base_addr_mapping[i]);
  }
  if (board_status.board_type == LP_PA100) {
    printf("\nHost FPGA board type is PA100, serial number %d\n", board_status.board_serial_number);
  } else if (board_status.board_type == LP_PA101) {
    printf("\nHost FPGA board type is PA101, serial number %d\n", board_status.board_serial_number);
  } else {
    printf("\nHost FPGA board type is unknown, serial number %d\n", board_status.board_serial_number);
  }
  printf("FPGA temperature %.2f C, power draw %.2f Watts\n", board_status.temp, board_status.power_draw);
  char display_buffer[512];
  printf("FPGA has had %ld power cycles, with a total alive time of %s\n", board_status.num_power_cycles, parse_seconds_to_days(board_status.time_alive_sec, display_buffer));
}

static void process_loop(struct launchpad_configuration * config, struct device_configuration* device_config,
        struct device_drivers * active_device_drivers, struct current_device_status * device_status) {
  if (device_config->communication_type == LP_DEVICE_COMM_UART) {
    interactive_uart(config, device_config, active_device_drivers, device_status);
  }
}

static void check_number_cores_on_device_and_active(struct launchpad_configuration * config, struct device_configuration * device_config) {
  if (config->all_cores_active) {
    for (int i=0;i<device_config->number_cores;i++) {
      config->active_cores[i]=true;
    }
  } else {
    for (int i=device_config->number_cores;i<MAX_NUM_CORES;i++) {
      if (config->active_cores[i]) {
        fprintf(stderr, "Error, core %d configured to be active but the device only has %d cores\n", i, device_config->number_cores);
        exit(-1);
      }
    }
    bool non_active=true;
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        non_active=false;
        break;
      }
    }
    if (non_active) {
      fprintf(stderr, "Error, device has %d cores but none configured to be active\n", device_config->number_cores);
      exit(-1);
    }
  }
}

static char* parse_seconds_to_days(uint64_t seconds, char * buffer) {
  long int remainder=seconds;
  int years=remainder / 31536000;
  remainder=remainder - (years * 31536000);
  int days=remainder / 86400;
  remainder=remainder - (days * 86400);
  int hours=remainder / 3600;
  remainder=remainder - (hours * 3600);
  int mins=remainder / 60;
  remainder=remainder - (mins * 60);
  if (years > 0) {
    sprintf(buffer,"%d years, %d days, %d hours, %d min and %ld secs", years, days, hours, mins, remainder);
  } else if (days > 0) {
    sprintf(buffer,"%d days, %d hours, %d min and %ld secs", days, hours, mins, remainder);
  } else {
    sprintf(buffer,"%d hours, %d min and %ld secs", hours, mins, remainder);
  }
  return buffer;
}
