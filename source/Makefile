CXX = c++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wpedantic

all: simulador

simulador: main.cpp riscv.cpp riscv.hpp
	$(CXX) $(CXXFLAGS) main.cpp riscv.cpp -o simulador

clean:
	$(RM) simulador simulador.exe

.PHONY: all clean
