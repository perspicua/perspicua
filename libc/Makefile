include ../common.mk

BUILD_DIR ?= build

CFLAGS = $(COMMON_CFLAGS) -Iinclude -I../kernel/include -I..

SRCS_COMMON = src/string.c src/printf.c
SRCS_KERNEL = kernel/hooks.c kernel/panic.c
SRCS_USER = user/hooks.c user/syscall.c

OBJS_K = $(patsubst %.c, $(BUILD_DIR)/%.libk.o, $(SRCS_COMMON) $(SRCS_KERNEL))
OBJS_U = $(patsubst %.c, $(BUILD_DIR)/%.libc.o, $(SRCS_COMMON) $(SRCS_USER))

all: $(BUILD_DIR)/libk.a $(BUILD_DIR)/libc.a

$(BUILD_DIR)/libk.a: $(OBJS_K)
	$(call print_ar,$@)
	$(Q)$(AR) rcs $@ $^

$(BUILD_DIR)/libc.a: $(OBJS_U)
	$(call print_ar,$@)
	$(Q)$(AR) rcs $@ $^

$(BUILD_DIR)/%.libk.o: %.c
	@mkdir -p $(dir $@)
	$(call print_cc,$< (kernel))
	$(Q)$(CC) $(CFLAGS) -D__KERNEL__ -c $< -o $@

$(BUILD_DIR)/%.libc.o: %.c
	@mkdir -p $(dir $@)
	$(call print_cc,$< (user))
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

clean:
	@printf "  $(COL_MAGENTA)CLEAN$(COL_DEFAULT)   libc\n"
	$(Q)rm -rf $(BUILD_DIR)

-include $(OBJS_K:.o=.d)
-include $(OBJS_U:.o=.d)
