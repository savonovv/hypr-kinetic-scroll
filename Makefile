PLUGIN_NAME = hypr-kinetic-scroll

CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b -g
LDFLAGS ?=

PKG_CONFIG = pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon

SRC = main.cpp kinetic.cpp
OUT = $(PLUGIN_NAME).so

all: $(OUT)

$(OUT): $(SRC) globals.hpp kinetic.hpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(SRC) -o $@ `$(PKG_CONFIG)`

clean:
	rm -f $(OUT)

.PHONY: all clean
