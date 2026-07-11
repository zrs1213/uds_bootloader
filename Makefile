# UDS Bootloader 协议栈 Makefile
#   make test    -> 本机(host)编译并运行全部单元测试
#   make arm     -> 交叉编译(arm-none-eabi-gcc)校验源码可在目标平台编译
#   make clean   -> 清理构建产物

CC      := gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Isrc
ARMCC   := arm-none-eabi-gcc
ARMFLAGS:= -std=c99 -Wall -Wextra -O2 -mcpu=cortex-m4 -mthumb -Isrc

SRC   := $(wildcard src/*.c)
TESTS := $(wildcard test/*.c)
TARGET := build/uds_test

.PHONY: test arm clean

test: $(TARGET)
	@echo ""
	./$(TARGET)

$(TARGET): $(SRC) $(TESTS)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(TESTS)

arm:
	@mkdir -p build/arm
	@echo "[arm] cross-compiling sources for Cortex-M4 ..."
	@for f in src/*.c; do \
		"$(ARMCC)" $(ARMFLAGS) -c "$$f" -o "build/arm/$$(basename $$f .c).o" || exit 1; \
	done
	@echo "[arm] OK: all sources compile for ARM thumb target"

clean:
	@rm -rf build
	@echo "[clean] done"
