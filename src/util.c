#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ncurses.h>
#include "util.h"
#include "launchpad_common.h"
#include "configuration.h"

static void load_executable_file(struct launchpad_configuration*, char**, uint64_t*);
static bool are_all_cores_active(struct launchpad_configuration*, struct device_configuration*);
static char* parse_seconds_to_days(uint64_t, char*);

void generate_device_configuration(struct device_configuration* device_config, struct device_drivers * active_device_drivers, char * target) {
  struct host_board_status board_status;
  check_device_status(active_device_drivers->device_get_host_board_status(&board_status));

  sprintf(target, "Device: '%s', version %x revision %d\n", device_config->device_name, device_config->version, device_config->revision);
  sprintf(target, "%sCPU configuration: %d cores of %s\n", target, device_config->number_cores, device_config->cpu_name);
  if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_NOTHING) {
    sprintf(target,"%sArchitecture type: Split instruction memory, split data memory\n", target);
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_INSTR_ONLY) {
    sprintf(target,"%sArchitecture type: Shared instruction memory, split data memory\n", target);
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_DATA_ONLY) {
    sprintf(target,"%sArchitecture type: Split instruction memory, shared data memory\n", target);
  } else if (device_config->architecture_type == LP_ARCH_TYPE_SHARED_EVERYTHING) {
    sprintf(target,"%sArchitecture type: Shared instruction memory, shared data memory\n", target);
  }
  sprintf(target, "%sClock frequency: %dMHz\n", target, device_config->clock_frequency_mhz);
  sprintf(target, "%sPCIe control BAR window: %d\n", target, device_config->pcie_bar_ctrl_window_index);
  sprintf(target, "%sMemory configuration: %dMB instruction, %dMB data per core, %dKB shared data\n", target, device_config->instruction_space_size_mb,
    device_config->per_core_data_space_mb, device_config->shared_data_space_kb);

  bool ddr_inuse[2]={false, false};
  for (int i=0;i<device_config->number_cores;i++) {
    ddr_inuse[device_config->ddr_bank_mapping[i]]=true;
  }

  sprintf(target, "%s\nDDR bank 0 in use: %s, DDR bank 1 in use: %s\n", target, ddr_inuse[0] ? "yes" : "no", ddr_inuse[1] ? "yes" : "no");

  for (int i=0;i<device_config->number_cores;i++) {
    sprintf(target, "%sCore %d: DDR bank %d, host-side base data address 0x%lx\n", target, i, device_config->ddr_bank_mapping[i], device_config->ddr_base_addr_mapping[i]);
  }
  if (board_status.board_type == LP_PA100) {
    sprintf(target, "%s\nHost FPGA board type is PA100, serial number %d\n", target, board_status.board_serial_number);
  } else if (board_status.board_type == LP_PA101) {
    sprintf(target, "%s\nHost FPGA board type is PA101, serial number %d\n", target, board_status.board_serial_number);
  } else {
    sprintf(target, "%s\nHost FPGA board type is unknown, serial number %d\n", target, board_status.board_serial_number);
  }
  sprintf(target, "%sFPGA temperature %.2f C, power draw %.2f Watts\n", target, board_status.temp, board_status.power_draw);
  char display_buffer[512];
  sprintf(target, "%sFPGA has had %ld power cycles, with a total alive time of %s\n", target, board_status.num_power_cycles, parse_seconds_to_days(board_status.time_alive_sec, display_buffer));
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

void transfer_executable_to_device(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
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

int start_cores(struct launchpad_configuration * config, struct device_configuration * device_config,
                          struct device_drivers * active_device_drivers, struct current_device_status * device_status) {
  if (are_all_cores_active(config, device_config)) {
    check_device_status(active_device_drivers->device_start_allcores());
    for (int i=0;i<device_config->number_cores;i++) device_status->cores_active[i]=true;
    device_status->running=true;
    return device_config->number_cores;
  } else {
    int num_active=0;
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        check_device_status(active_device_drivers->device_start_core(i));
        device_status->cores_active[i]=true;
        num_active++;
      } else {
        device_status->cores_active[i]=false;
      }
    }
    device_status->running=true;
    return num_active;
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

void check_device_status(LP_STATUS_CODE status_code) {
  if (status_code != LP_SUCCESS) {
    endwin();
    fprintf(stderr, "Error calling device function\n");
    raise(SIGABRT);
    exit(-1);
  }
}
