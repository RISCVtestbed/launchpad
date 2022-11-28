#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ncurses.h>
#include <stdbool.h>
#include <pthread.h>
#include "uart_interactive.h"
#include "launchpad_common.h"
#include "configuration.h"

#define MAX_BUFFER_SIZE 2048
#define OUT_PAUSED_BUFFER_SIZE 1048576

void * poll_uart_thread(void*);
static void write_uart_data(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, char);
static int get_number_active_cores(struct launchpad_configuration*, struct device_configuration*);
static void poll_core_for_uart(int core_id, struct device_drivers*, int, char**, unsigned int*, char *, unsigned int*);
static void check_device_status(LP_STATUS_CODE);
static bool handle_command(char*);

// Denotes whether we can update the screen or not (e.g. pause updates if in escape mode)
_Atomic bool screenUpdateOk;

struct ThreadArgsStruct {
  struct launchpad_configuration * config;
  struct device_configuration * device_config;
  struct device_drivers * active_device_drivers;
};

void interactive_uart(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
  struct ThreadArgsStruct * threadArgs=(struct ThreadArgsStruct*) malloc(sizeof(struct ThreadArgsStruct));
  threadArgs->config=config;
  threadArgs->device_config=device_config;
  threadArgs->active_device_drivers=active_device_drivers;
  
  screenUpdateOk=true;
  
  // Initialise ncurses
  initscr();
  cbreak();
  noecho();
  timeout(1);
  scrollok(stdscr, true);
  use_default_colors();
  start_color();

  init_pair(1, COLOR_WHITE, COLOR_RED);
  
  pthread_t threadId;    
  int err = pthread_create(&threadId, NULL, &poll_uart_thread, threadArgs);
  if (err) {
    fprintf(stderr, "Error calling device function\n");    
    raise(SIGABRT);
    exit(-1);
  }
  
  char command_buffer[50];
  bool escapeMode=false;
  int prev_row, prev_col, x_pos;
  while(1==1) {        
    char ch=getch();    
    if (ch != ERR) {
      if (ch == 27 && !escapeMode) {
        screenUpdateOk=false;
        escapeMode=true;
        x_pos=0;
        getyx(stdscr, prev_row, prev_col);
        move(LINES-1, 0);
        deleteln();        
      } else if (escapeMode && (ch == '\n' || ch == 27)) {
        escapeMode=false;
        if (x_pos > 0) {
          command_buffer[x_pos]='\0';
          bool command_recognised=handle_command(command_buffer);
          if (!command_recognised) {
            attron(COLOR_PAIR(1));
            mvprintw(LINES-1,0, "Error: Command not recognised");
            attroff(COLOR_PAIR(1));
            refresh();
          } else {
            deleteln();
          }
        }
        move(prev_row, prev_col);
        screenUpdateOk=true;
      } else {        
        printw("%c", ch);
        refresh();
        if (escapeMode) {
          command_buffer[x_pos]=ch;
          x_pos++;
        } else {
          // If not in escape mode then write the character to UART
          write_uart_data(config, device_config, active_device_drivers, ch);
        }        
      }
    }
  }
  pthread_join(threadId, NULL);
}

void * poll_uart_thread(void * args) {
  struct ThreadArgsStruct * threadArgs = (struct ThreadArgsStruct*) args;
  
  int num_active_cores=get_number_active_cores(threadArgs->config, threadArgs->device_config);
  char ** output_buffers=NULL;
  unsigned int * output_buffer_locals=NULL;
  if (num_active_cores > 1) {
    // Only buffer and output if number cores is greater than one, otherwise just dumb display
    output_buffers=(char**) malloc(sizeof(char*) * threadArgs->device_config->number_cores);
    output_buffer_locals=(unsigned int*) malloc(sizeof(unsigned int) * threadArgs->device_config->number_cores);
    for (int i=0;i<threadArgs->device_config->number_cores;i++) {
      if (threadArgs->config->active_cores[i]) {
        output_buffers[i]=(char*) malloc(sizeof(char) * MAX_BUFFER_SIZE);
        output_buffer_locals[i]=0;
      }
    }
  }  
  
  char * out_paused_buffer=(char*) malloc(sizeof(char*) * OUT_PAUSED_BUFFER_SIZE);
  unsigned int out_paused_buffer_idx=0;
  while (1==1) {
    for (int i=0;i<threadArgs->device_config->number_cores;i++) {
      if (threadArgs->config->active_cores[i]) {
        poll_core_for_uart(i, threadArgs->active_device_drivers, num_active_cores, output_buffers, output_buffer_locals, out_paused_buffer, &out_paused_buffer_idx);
      }
    }
  }
  return NULL;
}

static void write_uart_data(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers, char data) {
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) {
      active_device_drivers->device_write_uart(i, data);
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

static void poll_core_for_uart(int core_id, struct device_drivers * active_device_drivers, int num_active_cores, char ** output_buffers, unsigned int * output_buffer_locals,
      char * out_paused_buffer, unsigned int * out_paused_buffer_idx) {
  int uart_data_present=0;
  check_device_status(active_device_drivers->device_uart_has_data(core_id, &uart_data_present));
  if (*out_paused_buffer_idx > 0 && screenUpdateOk) {
    out_paused_buffer[*out_paused_buffer_idx]='\0';
    printw("%s", out_paused_buffer);
    refresh();    
    *out_paused_buffer_idx=0;
  }
  if (uart_data_present) {    
    char data=0x0;
    check_device_status(active_device_drivers->device_read_uart(core_id, &data));
    if (num_active_cores > 1) {
      if (output_buffer_locals[core_id] < MAX_BUFFER_SIZE) {
        if (data != '\r') {      
          output_buffers[core_id][output_buffer_locals[core_id]]=data;
          output_buffer_locals[core_id]++;
        }
        if (data == '\n') {
          // This is a flush
          if (screenUpdateOk) {
            output_buffers[core_id][output_buffer_locals[core_id]]='\0';
            printw("[%d]: %s", core_id, output_buffers[core_id]);
            refresh();
          } else {
            memcpy(&out_paused_buffer[*out_paused_buffer_idx], output_buffers[core_id], output_buffer_locals[core_id]);
            (*out_paused_buffer_idx)+=output_buffer_locals[core_id];
          }
          output_buffers[core_id][0]='\0';
          output_buffer_locals[core_id]=0;
        }
      } else {
        printf("Warning, UART buffer length of %d bytes exceeded for core %d, ignoring data '%c'\n", MAX_BUFFER_SIZE, core_id, data);
      }
    } else {      
      if (data != '\r') {
        if (screenUpdateOk) {          
          printw("%c", data);
          refresh();
        } else {
          out_paused_buffer[*out_paused_buffer_idx]=data;
          (*out_paused_buffer_idx)++;          
        }
      }      
    }
  }
}

static bool handle_command(char * buffer) {
  if (strcmp(buffer, ":q")==0) {
    endwin();
    exit(0);
  }

  return false;
}

static void check_device_status(LP_STATUS_CODE status_code) {
  if (status_code != LP_SUCCESS) {
    endwin();
    fprintf(stderr, "Error calling device function\n");    
    raise(SIGABRT);
    exit(-1);
  }
}
