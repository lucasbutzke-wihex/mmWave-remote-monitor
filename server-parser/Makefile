CC = clang
CFLAGS = -Wall -Wextra -std=c11 -O2 -g -fsanitize=address
# Add any libraries you need here (e.g., -lgpiod -lpthread)
LIBS = -lgpiod -lpthread -lrt -fsanitize=address

# Directories
SRC_DIR = .
BUILD_DIR = build

# Automatically find all .c files in the source directory
SRCS = $(wildcard $(SRC_DIR)/*.c)
# Convert source file paths into object file paths inside the build directory
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# Final binary output name
TARGET = $(BUILD_DIR)/server_parser

# Default target
all: $(TARGET)

# Link all object files into the final executable
$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(OBJS) -o $@ $(LIBS)

# Compile each .c file into a .o file
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

run:
	$(TARGET) /dev/ttyUSB0 /dev/ttyUSB1

# Clean up build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Phony targets
.PHONY: all clean