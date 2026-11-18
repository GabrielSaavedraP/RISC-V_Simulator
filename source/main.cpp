#include "riscv.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const std::array<std::string, 32> aliases = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

std::string hexValue(uint32_t value, int width = 8) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex
           << std::setfill('0') << std::setw(width) << value;
    return output.str();
}

uint64_t number(const std::string& token, uint64_t maximum) {
    std::string digits = token;
    int base = 10;
    if (digits.size() > 2 && digits[0] == '0'
        && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits = digits.substr(2);
    }
    uint64_t result = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(),
                                        result, base);
    if (digits.empty() || parsed.ec != std::errc()
        || parsed.ptr != digits.data() + digits.size() || result > maximum) {
        throw std::runtime_error("Numero fuera de rango o invalido: " + token);
    }
    return result;
}

uint32_t address(const std::string& token) {
    return static_cast<uint32_t>(number(token, UINT32_MAX));
}

unsigned registerIndex(const std::string& name) {
    if (name == "fp") {
        return 8;
    }
    for (unsigned index = 0; index < aliases.size(); ++index) {
        if (name == aliases[index]) {
            return index;
        }
    }
    if (name.size() > 1 && name[0] == 'x') {
        return static_cast<unsigned>(number(name.substr(1), 31));
    }
    throw std::runtime_error("Registro invalido: " + name);
}

std::vector<std::string> tokenize(const std::string& line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    input >> std::ws;
    while (!input.eof()) {
        std::string token;
        if (!(input >> std::quoted(token))) {
            throw std::runtime_error("Comillas sin cerrar.");
        }
        tokens.push_back(token);
        input >> std::ws;
    }
    return tokens;
}

void requireArguments(const std::vector<std::string>& tokens,
                      std::size_t minimum, std::size_t maximum,
                      const std::string& usage) {
    if (tokens.size() < minimum || tokens.size() > maximum) {
        throw std::runtime_error("Uso: " + usage);
    }
}

void printHelp() {
    std::cout
        << "load ARCHIVO       Carga un binario crudo en 0x00000000\n"
        << "step [N]           Ejecuta N instrucciones; por defecto, una\n"
        << "run [N]            Ejecuta hasta N instrucciones; por defecto, 100000\n"
        << "pc                 Muestra el contador de programa\n"
        << "regs [REG ...]     Muestra registros, por nombre x0-x31 o alias ABI\n"
        << "mem INICIO FIN     Muestra bytes, ambos extremos incluidos\n"
        << "words INICIO N     Muestra N palabras de 32 bits\n"
        << "break DIRECCION    Agrega un punto de parada\n"
        << "breaks             Muestra los puntos de parada\n"
        << "delete DIRECCION   Elimina un punto de parada\n"
        << "reset              Restaura el binario cargado y el estado inicial\n"
        << "status             Muestra PC, pasos y estado de ejecucion\n"
        << "help               Muestra esta ayuda\n"
        << "exit               Termina el simulador\n"
        << "Numeros decimales o hexadecimales con prefijo 0x.\n"
        << "Use comillas para rutas que contengan espacios.\n";
}

void printRegister(const rv::Machine& machine, unsigned index) {
    const uint32_t value = machine.registers()[index];
    const int64_t signedValue = value < 0x80000000u
        ? static_cast<int64_t>(value)
        : static_cast<int64_t>(value) - 0x100000000LL;
    std::cout << 'x' << index << " (" << aliases[index] << ") = "
              << hexValue(value) << "  " << signedValue << '\n';
}

void printMemory(const rv::Machine& machine, uint32_t first, uint32_t last) {
    if (last < first || uint64_t(last) - first + 1 > 65536) {
        throw std::runtime_error("El rango debe contener entre 1 y 65536 bytes.");
    }
    for (uint64_t cursor = first; cursor <= last; ++cursor) {
        if ((cursor - first) % 16 == 0) {
            std::cout << hexValue(static_cast<uint32_t>(cursor)) << "  ";
        }
        std::cout << hexValue(machine.byte(static_cast<uint32_t>(cursor)), 2).substr(2);
        if ((cursor - first) % 16 == 15 || cursor == last) {
            std::cout << '\n';
        } else {
            std::cout << ' ';
        }
    }
}

void printWords(const rv::Machine& machine, uint32_t first, uint64_t count) {
    if (count == 0 || count > 4096 || first % 4 != 0
        || uint64_t(first) + count * 4 > 0x100000000ULL) {
        throw std::runtime_error("Se requieren 1 a 4096 palabras alineadas dentro de la memoria.");
    }
    for (uint64_t index = 0; index < count; ++index) {
        const auto location = static_cast<uint32_t>(uint64_t(first) + index * 4);
        const uint32_t value = machine.word(location);
        const int64_t signedValue = value < 0x80000000u
            ? static_cast<int64_t>(value)
            : static_cast<int64_t>(value) - 0x100000000LL;
        std::cout << hexValue(location) << "  " << hexValue(value)
                  << "  " << signedValue << '\n';
    }
}

void execute(rv::Machine& machine, const std::set<uint32_t>& breakpoints,
             uint64_t limit, bool trace) {
    if (limit == 0) {
        throw std::runtime_error("La cantidad de instrucciones debe ser mayor que cero.");
    }
    uint64_t executed = 0;
    while (executed < limit && !machine.halted()) {
        if (!trace && breakpoints.count(machine.pc()) != 0) {
            std::cout << "Punto de parada en " << hexValue(machine.pc())
                      << ". Use step para continuar desde esta instruccion.\n";
            break;
        }
        const auto result = machine.step();
        ++executed;
        if (trace) {
            std::cout << "PC " << hexValue(result.pc)
                      << "  instruccion " << hexValue(result.instruction)
                      << "  siguiente PC " << hexValue(result.nextPc) << '\n';
        }
    }
    std::cout << executed << " instrucciones ejecutadas. PC = "
              << hexValue(machine.pc()) << '\n';
    if (machine.halted()) {
        std::cout << "Ejecucion detenida por EBREAK. Use reset para reiniciar.\n";
    } else if (!trace && executed == limit) {
        std::cout << "Limite de ejecucion alcanzado.\n";
    }
}

void command(rv::Machine& machine, std::set<uint32_t>& breakpoints,
             const std::vector<std::string>& tokens) {
    const std::string& operation = tokens.front();
    if (operation == "help") {
        requireArguments(tokens, 1, 1, "help");
        printHelp();
        return;
    }
    if (operation == "load") {
        requireArguments(tokens, 2, 2, "load ARCHIVO");
        machine.load(tokens[1]);
        breakpoints.clear();
        std::cout << machine.programSize() << " bytes cargados en 0x00000000.\n";
        return;
    }
    if (machine.programSize() == 0) {
        throw std::runtime_error("Primero cargue un programa con load ARCHIVO.");
    }
    if (operation == "pc" || operation == "status") {
        requireArguments(tokens, 1, 1, operation);
        std::cout << "PC = " << hexValue(machine.pc()) << '\n';
        if (operation == "status") {
            std::cout << "Instrucciones ejecutadas: " << machine.steps()
                      << "\nEstado: " << (machine.halted() ? "detenido" : "listo") << '\n';
        }
    } else if (operation == "regs") {
        if (tokens.size() == 1) {
            for (unsigned index = 0; index < 32; ++index) {
                printRegister(machine, index);
            }
        } else {
            std::vector<unsigned> selected;
            for (std::size_t index = 1; index < tokens.size(); ++index) {
                selected.push_back(registerIndex(tokens[index]));
            }
            for (unsigned index : selected) {
                printRegister(machine, index);
            }
        }
    } else if (operation == "mem") {
        requireArguments(tokens, 3, 3, "mem INICIO FIN");
        printMemory(machine, address(tokens[1]), address(tokens[2]));
    } else if (operation == "words") {
        requireArguments(tokens, 3, 3, "words INICIO N");
        printWords(machine, address(tokens[1]), number(tokens[2], 4096));
    } else if (operation == "step" || operation == "run") {
        requireArguments(tokens, 1, 2, operation + " [N]");
        const uint64_t count = tokens.size() == 2
            ? number(tokens[1], 10000000)
            : (operation == "step" ? 1 : 100000);
        execute(machine, breakpoints, count, operation == "step");
    } else if (operation == "break" || operation == "delete") {
        requireArguments(tokens, 2, 2, operation + " DIRECCION");
        const uint32_t location = address(tokens[1]);
        if (location % 4 != 0) {
            throw std::runtime_error("La direccion de instruccion debe ser multiplo de 4.");
        }
        if (operation == "break") {
            breakpoints.insert(location);
            std::cout << "Punto de parada agregado en " << hexValue(location) << '\n';
        } else {
            const bool removed = breakpoints.erase(location) != 0;
            std::cout << (removed ? "Punto de parada eliminado.\n"
                                  : "Esa direccion no tenia punto de parada.\n");
        }
    } else if (operation == "breaks") {
        requireArguments(tokens, 1, 1, "breaks");
        if (breakpoints.empty()) {
            std::cout << "Sin puntos de parada.\n";
        }
        for (uint32_t location : breakpoints) {
            std::cout << hexValue(location) << '\n';
        }
    } else if (operation == "reset") {
        requireArguments(tokens, 1, 1, "reset");
        machine.reset();
        std::cout << "Programa restaurado. PC y registros inicializados en cero.\n";
    } else {
        throw std::runtime_error("Comando desconocido. Use help.");
    }
}

}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        std::cerr << "Uso: " << argv[0] << " [programa.bin]\n";
        return 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
        printHelp();
        return 0;
    }
    rv::Machine machine;
    std::set<uint32_t> breakpoints;
    try {
        if (argc == 2) {
            machine.load(argv[1]);
            std::cout << machine.programSize() << " bytes cargados en 0x00000000.\n";
        }
        std::cout << "Simulador RISC-V RV32I. Escriba help para ver los comandos.\n";
        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line)) {
            try {
                const auto tokens = tokenize(line);
                if (tokens.empty()) {
                    continue;
                }
                if (tokens[0] == "exit" || tokens[0] == "quit") {
                    requireArguments(tokens, 1, 1, "exit");
                    break;
                }
                command(machine, breakpoints, tokens);
            } catch (const std::exception& error) {
                std::cerr << "Error: " << error.what() << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
