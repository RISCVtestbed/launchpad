#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "uart_io.h"
#include "launchpad_common.h"
#include "configuration.h"

#define MAX_BUFFER_SIZE 2048

static int get_number_active_cores(struct launchpad_configuration*, struct device_configuration*);
static void poll_core_for_uart(int core_id, struct device_drivers*, int, char**, unsigned int*);
static void check_device_status(LP_STATUS_CODE);

void poll_for_uart_across_cores(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
  int num_active_cores=get_number_active_cores(config, device_config);
  char ** output_buffers=NULL;
  unsigned int * output_buffer_locals=NULL;
  if (num_active_cores > 1) {
    // Only buffer and output if number cores is greater than one, otherwise just dumb display
    output_buffers=(char**) malloc(sizeof(char*) * device_config->number_cores);
    output_buffer_locals=(unsigned int*) malloc(sizeof(unsigned int) * device_config->number_cores);
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        output_buffers[i]=(char*) malloc(sizeof(char) * MAX_BUFFER_SIZE);
        output_buffer_locals[i]=0;
      }
    }
  }  
  while(1==1) {
    for (int i=0;i<device_config->number_cores;i++) {
      if (config->active_cores[i]) {
        poll_core_for_uart(i, active_device_drivers, num_active_cores, output_buffers, output_buffer_locals);
      }
    }
  }
}

static int get_number_active_cores(struct launchpad_configuration * config, struct device_configuration * device_config) {
  int num_active_cores=0;
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) num_active_cores++;
  }
  return num_active_cores;
}

static void poll_core_for_uart(int core_id, struct device_drivers * active_device_drivers, int num_active_cores, char ** output_buffers, unsigned int * output_buffer_locals) {
  int uart_data_present=0;
  check_device_status(active_device_drivers->device_uart_has_data(core_id, &uart_data_present));
  if (uart_data_present) {    
    char data=0x0;
    check_device_status(active_device_drivers->device_read_uart(core_id, &data));    
    if (num_active_cores > 1) {
      if (output_buffer_locals[core_id] < MAX_BUFFER_SIZE) {
        output_buffers[core_id][output_buffer_locals[core_id]]=data;
        output_buffer_locals[core_id]++;
        if (data == '\n') {
          // This is a flush
          output_buffers[core_id][output_buffer_locals[core_id]]='\0';    
          printf("[%d]: %s", core_id, output_buffers[core_id]);
          output_buffers[core_id][0]='\0';
          output_buffer_locals[core_id]=0;
        }
      } else {
        printf("Warning, UART buffer length of %d bytes exceeded for core %d, ignoring data '%c'\n", MAX_BUFFER_SIZE, core_id, data);
      }
    } else {
      printf("%c", data);
    }
  }
}

static void check_device_status(LP_STATUS_CODE status_code) {
  if (status_code != LP_SUCCESS) {
    fprintf(stderr, "Error calling device function\n");
    raise(SIGABRT);
    exit(-1);
  }
}
