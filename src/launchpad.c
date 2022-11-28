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

static int get_number_active_cores(struct launchpad_configuration*, struct device_configuration*);
static void process_loop(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*);
static void check_number_cores_on_device_and_active(struct launchpad_configuration*, struct device_configuration*);

int main(int argc, char * argv[]) {
  struct launchpad_configuration * config=readConfiguration(argc, argv);
  struct device_drivers active_device_drivers;
  struct device_configuration device_config;
  struct current_device_status device_status;

#ifdef MINOTAUR_SUPPORT
  active_device_drivers=setup_minotaur_device_drivers();
#endif

  if (config->reset) check_device_status(active_device_drivers.device_reset());
  
  check_device_status(active_device_drivers.device_initialise());
  device_status.initialised=true;
  check_device_status(active_device_drivers.device_get_configuration(&device_config));
  device_status.cores_active=(bool*) malloc(sizeof(bool) * device_config.number_cores);
  if (config->display_config) {
    char * config_str=(char*) malloc(sizeof(char) * CONFIGURATION_STR_SIZE);
    generate_device_configuration(&device_config, &active_device_drivers, config_str);
    printf("%s", config_str);
    free(config_str);
  }
  if (config->executable_filename != NULL && get_number_active_cores(config, &device_config) > 0) {
    check_number_cores_on_device_and_active(config, &device_config);
    transfer_executable_to_device(config, &device_config, &active_device_drivers);
    start_cores(config, &device_config, &active_device_drivers, &device_status);      
  }
  process_loop(config, &device_config, &active_device_drivers, &device_status);
  return 0;
}

static void process_loop(struct launchpad_configuration * config, struct device_configuration* device_config,
        struct device_drivers * active_device_drivers, struct current_device_status * device_status) {
  if (device_config->communication_type == LP_DEVICE_COMM_UART) {
    interactive_uart(config, device_config, active_device_drivers, device_status);
  }
}

static int get_number_active_cores(struct launchpad_configuration * config, struct device_configuration * device_config) {
  int num_active=0;
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) num_active++;
  }
  return num_active;
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
