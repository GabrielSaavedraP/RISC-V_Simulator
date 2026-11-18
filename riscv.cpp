#include "riscv.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rv {
namespace {

std::string hexadecimal(std::uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

std::uint32_t signExtend(std::uint32_t value, unsigned bits) {
    const std::uint32_t sign = std::uint32_t{1} << (bits - 1);
    return (value ^ sign) - sign;
}

bool signedLess(std::uint32_t left, std::uint32_t right) {
    return (left ^ 0x80000000u) < (right ^ 0x80000000u);
}

std::uint32_t arithmeticShift(std::uint32_t value, unsigned amount) {
    if (amount == 0) {
        return value;
    }
    const std::uint32_t fill = (value & 0x80000000u) != 0
        ? std::numeric_limits<std::uint32_t>::max() << (32 - amount)
        : 0u;
    return (value >> amount) | fill;
}

void validateTarget(std::uint32_t target) {
    if ((target & 3u) != 0) {
        throw std::runtime_error("Direccion de instruccion desalineada: " + hexadecimal(target));
    }
}

[[noreturn]] void invalidInstruction(std::uint32_t instruction, std::uint32_t pc) {
    throw std::runtime_error("Instruccion invalida " + hexadecimal(instruction)
                             + " en PC=" + hexadecimal(pc));
}

}

Machine::Memory Machine::initialMemory(const std::vector<std::uint8_t>& image) {
    Memory memory;
    for (std::size_t offset = 0; offset < image.size();) {
        Page page{};
        const std::size_t count = std::min<std::size_t>(4096, image.size() - offset);
        std::copy_n(image.data() + offset, count, page.data());
        memory.emplace(static_cast<std::uint32_t>(offset >> 12), std::move(page));
        offset += count;
    }
    return memory;
}

void Machine::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("No se pudo abrir el archivo: " + path);
    }
    const std::streamoff length = input.tellg();
    if (length <= 0 || static_cast<std::uint64_t>(length) > (std::uint64_t{1} << 32)
        || static_cast<std::uint64_t>(length) > std::numeric_limits<std::size_t>::max()
        || length > std::numeric_limits<std::streamsize>::max()) {
        throw std::runtime_error("El binario debe contener entre 1 y 4294967296 bytes.");
    }
    input.seekg(0);
    std::vector<std::uint8_t> image(static_cast<std::size_t>(length));
    if (!input.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("No se pudo leer el binario completo.");
    }
    if (image.size() >= 4 && image[0] == 0x7F && image[1] == 'E'
        && image[2] == 'L' && image[3] == 'F') {
        throw std::runtime_error("Se requiere un binario crudo; el archivo seleccionado es ELF.");
    }
    Memory memory = initialMemory(image);
    memory_.swap(memory);
    image_.swap(image);
    registers_.fill(0);
    pc_ = 0;
    steps_ = 0;
    halted_ = false;
}

void Machine::reset() {
    if (image_.empty()) {
        throw std::runtime_error("Primero debe cargar un programa.");
    }
    Memory memory = initialMemory(image_);
    memory_.swap(memory);
    registers_.fill(0);
    pc_ = 0;
    steps_ = 0;
    halted_ = false;
}

void Machine::validateAccess(std::uint32_t address, unsigned width) {
    if (static_cast<std::uint64_t>(address) + width > (std::uint64_t{1} << 32)) {
        throw std::runtime_error("Acceso fuera del espacio de memoria de 32 bits.");
    }
    if ((address & (width - 1u)) != 0) {
        throw std::runtime_error("Acceso de memoria desalineado en " + hexadecimal(address));
    }
}

std::uint32_t Machine::read(std::uint32_t address, unsigned width) const {
    validateAccess(address, width);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        value |= static_cast<std::uint32_t>(byte(address + i)) << (8u * i);
    }
    return value;
}

void Machine::write(std::uint32_t address, unsigned width, std::uint32_t value) {
    validateAccess(address, width);
    Page& page = memory_[address >> 12];
    const std::size_t offset = address & 0xFFFu;
    for (unsigned i = 0; i < width; ++i) {
        page[offset + i] = static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu);
    }
}

StepResult Machine::step() {
    if (image_.empty()) {
        throw std::runtime_error("Primero debe cargar un programa.");
    }
    if (halted_) {
        throw std::runtime_error("El programa esta detenido por EBREAK. Use reset o load.");
    }
    validateTarget(pc_);
    const std::uint32_t instruction = word(pc_);
    const std::uint32_t opcode = instruction & 0x7Fu;
    const unsigned rd = (instruction >> 7) & 31u;
    const unsigned funct3 = (instruction >> 12) & 7u;
    const unsigned rs1 = (instruction >> 15) & 31u;
    const unsigned rs2 = (instruction >> 20) & 31u;
    const unsigned funct7 = instruction >> 25;
    const std::uint32_t left = registers_[rs1];
    const std::uint32_t right = registers_[rs2];
    const std::uint32_t immediate = signExtend(instruction >> 20, 12);
    std::uint32_t nextPc = pc_ + 4u;
    std::uint32_t result = 0;
    bool registerWrite = false;
    unsigned storeWidth = 0;
    std::uint32_t storeAddress = 0;
    bool stop = false;

    switch (opcode) {
        case 0x03: {
            const std::uint32_t address = left + immediate;
            switch (funct3) {
                case 0: result = signExtend(read(address, 1), 8); break;
                case 1: result = signExtend(read(address, 2), 16); break;
                case 2: result = read(address, 4); break;
                case 4: result = read(address, 1); break;
                case 5: result = read(address, 2); break;
                default: invalidInstruction(instruction, pc_);
            }
            registerWrite = true;
            break;
        }
        case 0x13:
            switch (funct3) {
                case 0: result = left + immediate; break;
                case 1:
                    if (funct7 != 0) {
                        invalidInstruction(instruction, pc_);
                    }
                    result = left << rs2;
                    break;
                case 2: result = static_cast<std::uint32_t>(signedLess(left, immediate)); break;
                case 3: result = static_cast<std::uint32_t>(left < immediate); break;
                case 4: result = left ^ immediate; break;
                case 5:
                    if (funct7 == 0) {
                        result = left >> rs2;
                    } else if (funct7 == 0x20) {
                        result = arithmeticShift(left, rs2);
                    } else {
                        invalidInstruction(instruction, pc_);
                    }
                    break;
                case 6: result = left | immediate; break;
                case 7: result = left & immediate; break;
            }
            registerWrite = true;
            break;
        case 0x17:
            result = pc_ + (instruction & 0xFFFFF000u);
            registerWrite = true;
            break;
        case 0x23: {
            const std::uint32_t offset = signExtend(((instruction >> 25) << 5)
                                                   | ((instruction >> 7) & 31u), 12);
            if (funct3 > 2) {
                invalidInstruction(instruction, pc_);
            }
            storeWidth = 1u << funct3;
            storeAddress = left + offset;
            validateAccess(storeAddress, storeWidth);
            break;
        }
        case 0x33:
            if (funct7 != 0 && !(funct7 == 0x20 && (funct3 == 0 || funct3 == 5))) {
                invalidInstruction(instruction, pc_);
            }
            switch (funct3) {
                case 0: result = funct7 == 0 ? left + right : left - right; break;
                case 1: result = left << (right & 31u); break;
                case 2: result = static_cast<std::uint32_t>(signedLess(left, right)); break;
                case 3: result = static_cast<std::uint32_t>(left < right); break;
                case 4: result = left ^ right; break;
                case 5:
                    result = funct7 == 0 ? left >> (right & 31u)
                                         : arithmeticShift(left, right & 31u);
                    break;
                case 6: result = left | right; break;
                case 7: result = left & right; break;
            }
            registerWrite = true;
            break;
        case 0x37:
            result = instruction & 0xFFFFF000u;
            registerWrite = true;
            break;
        case 0x63: {
            const std::uint32_t offset = signExtend(((instruction >> 31) << 12)
                | (((instruction >> 7) & 1u) << 11)
                | (((instruction >> 25) & 63u) << 5)
                | (((instruction >> 8) & 15u) << 1), 13);
            bool taken = false;
            switch (funct3) {
                case 0: taken = left == right; break;
                case 1: taken = left != right; break;
                case 4: taken = signedLess(left, right); break;
                case 5: taken = !signedLess(left, right); break;
                case 6: taken = left < right; break;
                case 7: taken = left >= right; break;
                default: invalidInstruction(instruction, pc_);
            }
            if (taken) {
                nextPc = pc_ + offset;
                validateTarget(nextPc);
            }
            break;
        }
        case 0x67:
            if (funct3 != 0) {
                invalidInstruction(instruction, pc_);
            }
            nextPc = (left + immediate) & ~std::uint32_t{1};
            validateTarget(nextPc);
            result = pc_ + 4u;
            registerWrite = true;
            break;
        case 0x6F: {
            const std::uint32_t offset = signExtend(((instruction >> 31) << 20)
                | (((instruction >> 12) & 255u) << 12)
                | (((instruction >> 20) & 1u) << 11)
                | (((instruction >> 21) & 1023u) << 1), 21);
            nextPc = pc_ + offset;
            validateTarget(nextPc);
            result = pc_ + 4u;
            registerWrite = true;
            break;
        }
        case 0x73:
            if (instruction == 0x00100073u) {
                stop = true;
                nextPc = pc_;
            } else if (instruction == 0x00000073u) {
                throw std::runtime_error("ECALL requiere un entorno de sistema no implementado.");
            } else {
                invalidInstruction(instruction, pc_);
            }
            break;
        default:
            invalidInstruction(instruction, pc_);
    }

    if (storeWidth != 0) {
        write(storeAddress, storeWidth, right);
    }
    if (registerWrite && rd != 0) {
        registers_[rd] = result;
    }
    const StepResult execution{pc_, instruction, nextPc, stop};
    pc_ = nextPc;
    halted_ = stop;
    ++steps_;
    return execution;
}

std::uint32_t Machine::pc() const {
    return pc_;
}

const std::array<std::uint32_t, 32>& Machine::registers() const {
    return registers_;
}

std::uint8_t Machine::byte(std::uint32_t address) const {
    const auto page = memory_.find(address >> 12);
    return page == memory_.end() ? std::uint8_t{0} : page->second[address & 0xFFFu];
}

std::uint32_t Machine::word(std::uint32_t address) const {
    return read(address, 4);
}

bool Machine::halted() const {
    return halted_;
}

std::uint64_t Machine::steps() const {
    return steps_;
}

std::size_t Machine::programSize() const {
    return image_.size();
}

}
