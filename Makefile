CC = cc
CFLAGS = -Wall -Wextra -O2 -Ilib
LDLIBS = -ldl -lpthread -lm

TARGET = azipod
MINIAUDIO_OBJ = lib/miniaudio/miniaudio.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): azipod.o $(MINIAUDIO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

azipod.o: azipod.c lib/miniaudio/miniaudio.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(MINIAUDIO_OBJ): lib/miniaudio/miniaudio.c lib/miniaudio/miniaudio.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) azipod.o $(MINIAUDIO_OBJ)
