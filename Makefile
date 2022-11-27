
CC=gcc
CFLAGS=-g -Iinclude
LFLAGS=-lncurses -lpthread

EXE_FILE=launchpad

LP_SRCDIR   = src
OBJDIR   = build

LP_SOURCES  := $(wildcard $(LP_SRCDIR)/*.c)
LP_OBJECTS  := $(LP_SOURCES:$(LP_SRCDIR)/%.c=$(OBJDIR)/%.o)

ADXDMA_LOC=/store/nbrown23/alpha-data/pa100/sdk/admpa100_sdk-1.1.0/host/adxdma-v0_11_0

minotaur: CFLAGS+=-DMINOTAUR_SUPPORT -I$(DEVICE_SRC_DIR)
minotaur: LFLAGS+=-ladxdma
minotaur: check-env build_buildDir $(LP_OBJECTS)
	$(CC) $(CFLAGS) -I$(ADXDMA_LOC)/include -c $(DEVICE_SRC_DIR)/minotaur.c -o $(OBJDIR)/minotaur.o
	$(CC) -o $(EXE_FILE) $(LP_OBJECTS) $(OBJDIR)/minotaur.o $(LFLAGS)
	
$(LP_OBJECTS): $(OBJDIR)/%.o : $(LP_SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

build_buildDir:
	@mkdir -p $(OBJDIR)

check-env:
ifndef DEVICE_SRC_DIR
	$(error DEVICE_SRC_DIR is undefined, set as environment variable)
endif
