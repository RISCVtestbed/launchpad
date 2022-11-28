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
