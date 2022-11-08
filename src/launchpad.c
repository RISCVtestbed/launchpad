#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include "launchpad_common.h"
#include "configuration.h"
#include "uart_io.h"

#ifdef MINOTAUR_SUPPORT
#include "minotaur.h"
#endif

static void display_device_configuration(struct device_configuration*, struct device_drivers*);
static void process_loop(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*);
static void start_cores(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*);
static bool are_all_cores_active(struct launchpad_configuration*, struct device_configuration*);
static void check_number_cores_on_device_and_active(struct launchpad_configuration*, struct device_configuration*);
static void transfer_executable_to_device(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*);
static void load_executable_file(struct launchpad_configuration*, char**, uint64_t*);
static void check_device_status(LP_STATUS_CODE);
static char* parse_seconds_to_days(uint64_t, char*);

int main(int argc, char * argv[]) {
  struct launchpad_configuration * config=readConfiguration(argc, argv);
  struct device_drivers active_device_drivers;
  struct device_configuration device_config;

#ifdef MINOTAUR_SUPPORT
  active_device_drivers=setup_minotaur_device_drivers();
#endif

  if (config->reset) check_device_status(active_device_drivers.device_reset());

  if (config->executable_filename != NULL || config->display_config) {
    check_device_status(active_device_drivers.device_initialise());
    check_device_status(active_device_drivers.device_get_configuration(&device_config));
    if (config->display_config) display_device_configuration(&device_config, &active_device_drivers);
    if (config->executable_filename != NULL) {
      check_number_cores_on_device_and_active(config, &device_config);
      transfer_executable_to_device(config, &device_config, &active_device_drivers);
      start_cores(config, &device_config, &active_device_drivers);
      process_loop(config, &device_config, &active_device_drivers);
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

static void process_loop(struct launchpad_configuration * config, struct device_configuration* device_config, struct device_drivers * active_device_drivers) {
  if (config->poll_uart) {
    poll_for_uart_across_cores(config, device_config, active_device_drivers);
  }
}

static void start_cores(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
  if (1==2 && are_all_cores_active(config, device_config)) {
    check_device_status(active_device_drivers->device_start_allcores());
  } else {
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        check_device_status(active_device_drivers->device_start_core(i));
      }
    }
  }
}

static bool are_all_cores_active(struct launchpad_configuration * config, struct device_configuration * device_config) {
  for (int i=0;i<device_config->number_cores;i++) {
    if (!config->active_cores[i]) {
      return false;
    }
  }
  return true;
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

static void transfer_executable_to_device(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
  char * executable_bytes;
  uint64_t code_size;
  load_executable_file(config, &executable_bytes, &code_size);
  if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_NOTHING || device_config->architecture_type == LP_ARCH_TYPE_SHARED_DATA_ONLY) {
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        check_device_status(active_device_drivers->device_write_core_instructions(i, 0x0, executable_bytes, code_size));
      }
    }
  } else {
    // Otherwise there is a shared instruction space
    check_device_status(active_device_drivers->device_write_instructions(0x0, executable_bytes, code_size));
  }
  free(executable_bytes);
}

static void load_executable_file(struct launchpad_configuration * config, char **exec_buffer, uint64_t * code_size) {
  int handle=open(config->executable_filename, O_RDONLY);
  if (handle == -1) {
    fprintf(stderr, "Error opening executable file '%s', check it exists\n", config->executable_filename);
    exit(-1);
  }
  struct stat st;
  int err=fstat(handle, &st);
  if (err == -1) {
    fprintf(stderr, "Error obtaining status on executable file '%s'\n", config->executable_filename);
    close(handle);
    exit(-1);
  }

  *code_size = (uint64_t) st.st_size;
  *exec_buffer=(char*) malloc(*code_size);
  err=read(handle, *exec_buffer, *code_size);
  if (err == -1) {
    fprintf(stderr, "Error reading executable file '%s'\n", config->executable_filename);
    close(handle);
    exit(-1);
  }
  close(handle);
}

static void check_device_status(LP_STATUS_CODE status_code) {
  if (status_code != LP_SUCCESS) {
    fprintf(stderr, "Error calling device function\n");
    raise(SIGABRT);
    exit(-1);
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
