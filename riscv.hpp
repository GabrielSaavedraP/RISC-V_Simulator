#ifndef RISCV_HPP
#define RISCV_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rv {

struct StepResult {
    std::uint32_t pc;
    std::uint32_t instruction;
    std::uint32_t nextPc;
    bool halted;
};

class Machine {
public:
    void load(const std::string& path);
    void reset();
    StepResult step();
    std::uint32_t pc() const;
    const std::array<std::uint32_t, 32>& registers() const;
    std::uint8_t byte(std::uint32_t address) const;
    std::uint32_t word(std::uint32_t address) const;
    bool halted() const;
    std::uint64_t steps() const;
    std::size_t programSize() const;

private:
    using Page = std::array<std::uint8_t, 4096>;
    using Memory = std::unordered_map<std::uint32_t, Page>;

    static Memory initialMemory(const std::vector<std::uint8_t>& image);
    static void validateAccess(std::uint32_t address, unsigned width);
    std::uint32_t read(std::uint32_t address, unsigned width) const;
    void write(std::uint32_t address, unsigned width, std::uint32_t value);

    Memory memory_;
    std::vector<std::uint8_t> image_;
    std::array<std::uint32_t, 32> registers_{};
    std::uint32_t pc_ = 0;
    std::uint64_t steps_ = 0;
    bool halted_ = false;
};

}

#endif
