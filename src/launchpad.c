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

static void process_loop(struct launchpad_configuration*, struct device_drivers);
static void start_cores(struct launchpad_configuration*, struct device_drivers);
static bool are_all_cores_active(struct launchpad_configuration*);
static void check_number_cores_on_device_and_active(struct launchpad_configuration*);
static void transfer_executable_to_device(struct launchpad_configuration*, struct device_configuration, struct device_drivers);
static void load_executable_file(struct launchpad_configuration*, char**, uint64_t*);
static void check_device_status(LP_STATUS_CODE);

int main(int argc, char * argv[]) {
  struct launchpad_configuration * config=readConfiguration(argc, argv);
  struct device_drivers active_device_drivers;
  struct device_configuration device_config;
  
#ifdef MINOTAUR_SUPPORT
  active_device_drivers=setup_minotaur_device_drivers();
#endif

  if (config->reset) check_device_status(active_device_drivers.device_reset());

  check_device_status(active_device_drivers.device_initialise());
  check_device_status(active_device_drivers.device_get_configuration(&device_config));
  config->total_cores_on_device=device_config.number_cores;
  check_number_cores_on_device_and_active(config);
  transfer_executable_to_device(config, device_config, active_device_drivers);
  start_cores(config, active_device_drivers);
  process_loop(config, active_device_drivers);
  return 0;
}

static void process_loop(struct launchpad_configuration * config, struct device_drivers active_device_drivers) {
  if (config->poll_uart) {
    poll_for_uart_across_cores(config, active_device_drivers);
  }
}

static void start_cores(struct launchpad_configuration * config, struct device_drivers active_device_drivers) {
  if (1==2 && are_all_cores_active(config)) {
    check_device_status(active_device_drivers.device_start_allcores());
  } else {
    for (int i=0;i<config->total_cores_on_device;i++) {
      if (config->active_cores[i]) {
        check_device_status(active_device_drivers.device_start_core(i));
      }
    }
  }  
}

static bool are_all_cores_active(struct launchpad_configuration * config) {
  for (int i=0;i<config->total_cores_on_device;i++) {
    if (!config->active_cores[i]) {
      return false;
    }
  }
  return true;
}

static void check_number_cores_on_device_and_active(struct launchpad_configuration * config) {
  if (config->all_cores_active) {
    for (int i=0;i<config->total_cores_on_device;i++) {
      config->active_cores[i]=true;
    }
  } else {
    for (int i=config->total_cores_on_device;i<MAX_NUM_CORES;i++) {
      if (config->active_cores[i]) {
        fprintf(stderr, "Error, core %d configured to be active but the device only has %d cores\n", i, config->total_cores_on_device);
        exit(-1);
      }
    }
    bool non_active=true;
    for (int i=0;i<config->total_cores_on_device;i++) {
      if (config->active_cores[i]) {
        non_active=false;
        break;
      }
    }
    if (non_active) {
      fprintf(stderr, "Error, device has %d cores but none configured to be active\n", config->total_cores_on_device);
      exit(-1);
    }
  }
}

static void transfer_executable_to_device(struct launchpad_configuration * config, struct device_configuration device_config, struct device_drivers active_device_drivers) {
  char * executable_bytes;
  uint64_t code_size;
  load_executable_file(config, &executable_bytes, &code_size);
  if (device_config.architecture_type == LP_ARCH_TYPE_SHARED_NOTHING || device_config.architecture_type == LP_ARCH_TYPE_SHARED_DATA_ONLY) {
    for (int i=0;i<config->total_cores_on_device;i++) {
      if (config->active_cores[i]) {
        check_device_status(active_device_drivers.device_write_core_instructions(i, 0x0, executable_bytes, code_size));
      }
    }
  } else {
    // Otherwise there is a shared instruction space    
    check_device_status(active_device_drivers.device_write_instructions(0x0, executable_bytes, code_size));
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
