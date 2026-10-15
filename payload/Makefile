PS5_HOST ?= 192.168.0.36
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined — compila dentro il container ps5sdk)
endif

ELF    := pscapture.elf
CFLAGS := -Wall -g -O2 -mssse3

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all clean deploy
