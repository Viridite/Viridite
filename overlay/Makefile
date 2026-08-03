#---------------------------------------------------------------------------------
# Viridite install overlay — a Tesla (.ovl) overlay applet.
#
# Built exactly like an NRO and then renamed: an .ovl *is* an NRO, it just gets
# loaded by nx-ovlloader into the overlay applet slot instead of by hbloader.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET      := Viridite
BUILD       := build
SOURCES     := source
INCLUDES    := include

APP_TITLE   := Viridite
APP_AUTHOR  := Viridite
APP_VERSION := 1.0.0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fexceptions -std=gnu++20

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
           -Wl,-Map,$(notdir $*.map)

LIBS    := -lnx

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).nro
	@cp $(OUTPUT).nro $(OUTPUT).ovl
	@echo built ... $(notdir $@)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
