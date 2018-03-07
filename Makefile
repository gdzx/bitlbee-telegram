BUILD_DIR = build
SRC_DIRS = src
LIBS = bitlbee glib-2.0
INC_DIRS = $(shell find $(SRC_DIRS) -type d)
TARGET_LIB = telegram.so

INC_FLAGS = $(addprefix -I,$(INC_DIRS))
CFLAGS = $(shell pkg-config --cflags $(LIBS)) $(INC_FLAGS) -fPIC -Wall -Wextra -Werror -O3
LDFLAGS = $(shell pkg-config --libs $(LIBS)) -ltdjson -shared

SRCS = $(shell find $(SRC_DIRS) -name *.c)
OBJS = $(SRCS:%=$(BUILD_DIR)/%.o)
DEPS = $(OBJS:.o=.d)

.PHONY: all
all: $(BUILD_DIR)/$(TARGET_LIB)

$(BUILD_DIR)/$(TARGET_LIB): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.c.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

.PHONY: clean
clean:
	-rm -rf $(BUILD_DIR)
