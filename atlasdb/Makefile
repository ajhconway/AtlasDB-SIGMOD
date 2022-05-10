.DEFAULT_GOAL := release

CC = clang
LIBAIO_INC_DIR =
LIBAIO_LIB_DIR =
LTO_FLAGS = -flto

#*************************************************************#
# DIRECTORIES, SRC, OBJ, ETC
#

SRCDIR= src
PLATFORMDIR= src/platform_linux
SRC= $(wildcard $(SRCDIR)/*.c $(PLATFORMDIR)/*.c)
TESTSRC= $(PLATFORMDIR)/driver_test.c

OBJDIR= obj
BINDIR= bin
DIRS= $(OBJDIR) $(BINDIR) $(DEPDIR) $(OBJDIR)/platform_linux $(DEPDIR)/platform_linux
TESTOBJ= $(TESTSRC:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
OBJ := $(filter-out $(TESTOBJ), $(SRC:$(SRCDIR)/%.c=$(OBJDIR)/%.o))
OBJ += xxHash/xxhash.o

TESTS= $(TESTSRC:$(PLATFORMDIR)/%.c=%)


#*************************************************************#
# CFLAGS, ETC
#

INCLUDE =
INCLUDE += -I $(SRCDIR)
INCLUDE += -I $(PLATFORMDIR)
INCLUDE += -I xxHash
INCLUDE += $(LIBAIO_INC_DIR)

# HACK: You can override this directory via make commandline if it's not what
# you want.  We'll need permission to make subdirectories.
DEFAULT_CFLAGS += -D_GNU_SOURCE -ggdb3 -Wall -pthread -Wfatal-errors -Werror
DEFAULT_CFLAGS += -msse4.2 -mpopcnt -DXXH_STATIC_LINKING_ONLY

# Warn for large stacks
# Generic options for both clang and gcc:
STACK_CFLAGS += -Wframe-larger-than=4096

# Enable if we want to fully remove variable-length-arrays:
STACK_CFLAGS += -Wvla

# gcc-only options:
ifneq (,$(findstring gcc,$(CC)))
   ifneq (,$(findstring clang,$(CC)))
      # confused about which compiler
      $(error Cannot tell what compiler)
   endif
   # GCC doesn't understand many of the attributes that clang uses
   DEFAULT_CFLAGS += -Wno-attributes

   # gcc-only options for alloca
   # Warn on use of allocas
   # alloca cannot be poisoned as it is a macro.
   # >= 1 byte is the smallest limit allowed so we cannot catch alloca(1)
   STACK_CFLAGS += -Walloca-larger-than=1
endif

DEFAULT_CFLAGS += $(STACK_CFLAGS)
#DEFAULT_CFLAGS += -fsanitize=memory -fsanitize-memory-track-origins
#DEFAULT_CFLAGS += -fsanitize=address
#DEFAULT_CFLAGS += -fsanitize=integer

CFLAGS += $(DEFAULT_CFLAGS) -Ofast $(LTO_FLAGS) -march=native
DEFAULT_LDFLAGS = -ggdb3 -pthread $(LIBAIO_LIB_DIR)
LDFLAGS = $(DEFAULT_LDFLAGS) -Ofast $(LTO_FLAGS)
LIBS = -lpthread -laio -lm

release: .release all
	rm -f .debug
	rm -f .debug-log

debug: CFLAGS = -g $(DEFAULT_CFLAGS)
debug: LDFLAGS = -g $(DEFAULT_LDFLAGS)
debug: .debug all
	rm -f .release
	rm -f .debug-log

debug-log: CFLAGS = -g -DCC_LOG $(DEFAULT_CFLAGS)
debug-log: LDFLAGS = -g $(DEFAULT_LDFLAGS)
debug-log: .debug-log all
	rm -f .release
	rm -f .debug

.release:
	$(MAKE) clean
	touch .release

.debug:
	$(MAKE) clean
	touch .debug

.debug-log:
	$(MAKE) clean
	touch .debug-log


#*************************************************************#
# DEPENDANCY

DEPDIR := $(OBJDIR)/.deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDE) $(TARGET_ARCH) -c


# Every c file must:
# - include platform.h
# - include poison.h
# - platform.h must be the first include
# - poison.h must be the last include
# To make it easier for users to know what failed, each of the above
# will have a different target name so you know which rule failed.
$(OBJDIR)/%.includes_platform.h: $(SRCDIR)/% | $(DIRS)
	@grep '^# *include *"platform.h"' $< > /dev/null || \
		echo	"ERROR: $< does not #include platform.h"
	@grep '^# *include *"platform.h"' $< > /dev/null
	@touch $@

$(OBJDIR)/%.includes_poison.h: $(SRCDIR)/% | $(DIRS)
	@grep '^# *include *"poison.h"' $< > /dev/null || \
		echo	"ERROR: $< does not #include poison.h"
	@grep '^# *include *"poison.h"' $< > /dev/null
	@touch $@

$(OBJDIR)/%.platform.h_is_first: $(SRCDIR)/% $(OBJDIR)/%.includes_platform.h
	@grep '^# *include' $< | head -1 | grep '"platform.h"' > /dev/null || \
		echo	"ERROR: $< : platform.h is not the first #include"
	@grep '^# *include' $< | head -1 | grep '"platform.h"' > /dev/null
	@touch $@

$(OBJDIR)/%.poison.h_is_last: $(SRCDIR)/% $(OBJDIR)/%.includes_platform.h
	@grep '^# *include' $< | tail -1 | grep '"poison.h"' > /dev/null || \
		echo	"ERROR: $< : poison.h is not the last #include"
	@grep '^# *include' $< | tail -1 | grep '"poison.h"' > /dev/null
	@touch $@

$(OBJDIR)/%.include_check: $(OBJDIR)/%.c.poison.h_is_last \
									$(OBJDIR)/%.c.platform.h_is_first
	@touch $@

# Ignore ycsb test for the include check
$(OBJDIR)/ycsb_test.include_check:
	@touch $@

$(OBJDIR)/%.o : $(SRCDIR)/%.c $(OBJDIR)/%.include_check $(DEPDIR)/%.d | $(DIRS)
	$(COMPILE.c) $(OUTPUT_OPTION) $<

DEPFILES := $(SRC:$(SRCDIR)/%.c=$(DEPDIR)/%.d)
$(DEPFILES):
include $(wildcard $(DEPFILES))

print-%  : ; @echo $* = $($*)


#*************************************************************#
# RECIPES
#

all : xxHash/xxhash.o $(OBJDIR) $(TESTS)


# Do not care about frame sizes for ycsb
ycsb_test: STACK_CFLAGS =
$(TESTS) : % : $(OBJDIR)/platform_linux/%.o $(DEPDIR)/platform_linux/%.d \
					$(OBJ) | $(DIRS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) $(INCLUDE) $< -o $(BINDIR)/$@ $(LIBS)

xxHash/xxhash.o : xxHash/xxhash.c xxHash/xxhash.h xxHash/xxh3.h
	$(MAKE) -C xxHash

$(DIRS):
	@mkdir -p $@


#*************************************************************#

.PHONY : cleand
clean :
	rm -rf $(OBJDIR)/*
	rm -rf $(BINDIR)/*
