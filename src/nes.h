#include <cstdint>
#include <bitset>
#include <vector>
#include <fstream>

struct iNESFile
{
    std::uint32_t magic_number;
    std::uint16_t mapper_id_and_flags;
    std::uint8_t PRG_ram_page_count, flag_byte; 
    std::vector<int8_t> PRG, CHR;

    iNESFile(std::string fl_name)
    {
        std::ifstream fl(fl_name, std::ios::binary);
        if(!fl)
            throw std::runtime_error("Couldn't open file: " + fl_name);

        std::uint8_t PRG_size, CHR_size;
        std::uint8_t junk[6];
        fl.read((char*)&magic_number, 4);
        fl.read((char*)&PRG_size, 1);
        fl.read((char*)&CHR_size, 1);
        fl.read((char*)&mapper_id_and_flags, 2);
        fl.read((char*)&PRG_ram_page_count, 1);
        fl.read((char*)&flag_byte, 1);
        fl.read((char*)junk, 6);

        // Read in actual ROM data.
        PRG.resize(PRG_size);
        fl.read((char*)PRG.data(), PRG_size);

        CHR.resize(CHR_size);
        fl.read((char*)CHR.data(), CHR_size);
    }
};

class NES
{
private:
    struct 
    {
        std::uint16_t PC;  // Program counter
        std::int8_t A;    // Accumulator 
        std::int8_t X, Y; // Indexers
        std::uint8_t S;    // Stack pointer
        bool carry;
        bool zero;
        bool interrupt_disable;
        bool decimal_mode;
        bool break_command;
        bool overflow;
        bool negative;
    } reg; // Registers

    enum AddressingMode 
    {
        Accumulator, Immediate, ZeroPage, ZeroPageX, Relative, Absolute, AbsoluteX, 
        AbsoluteY, Indirect, IndexedIndirect, IndirectIndexed
    } addr_mode;
    std::uint8_t opcode;
    std::uint16_t param;
    std::int8_t mem[65536]; // Internal memory.

    std::int8_t* stk;  // Stack.

    std::uint8_t get_processor_status_byte () 
    {
        std::uint8_t status;
        status += (int)reg.carry;
        status += (int)reg.zero << 1; 
        status += (int)reg.interrupt_disable << 2;
        status += (int)reg.decimal_mode << 3;
        status += (int)reg.break_command << 4;
        status += (int)reg.overflow << 5;
        status += (int)reg.negative << 6;
        return status;
    }

    void set_processor_status_byte (std::int8_t status) 
    {
        reg.carry = status & 1;
        reg.zero = (status >> 1) & 1;
        reg.interrupt_disable = (status >> 1) & 1;
        reg.decimal_mode = (status >> 1) & 1;
        reg.break_command = (status >> 1) & 1;
        reg.overflow = (status >> 1) & 1;
        reg.negative = (status >> 1) & 1;
    }

    void push_stack (std::int8_t val) 
    {
        *stk++ = val;
#ifdef DEBUG 
        if(stk - mem > 0x01FF)
            std::cerr << "Oh no: Stack overflow!! " << (stk - mem) << std::endl;
#endif
    }
    std::int8_t pop_stack () 
    {
#ifdef DEBUG 
        if(stk - mem <= 0x0100)
            std::cerr << "Oh no: Stack underflow!! " << (stk - mem) << std::endl;
#endif
        return *stk--;
    }

    std::int8_t get_8_bit_param () 
    {
        return param & 0xFF;
    }

    std::int8_t get_M () 
    {
        // Use param, not the actual memory for M in Immediate mode.
        if(addr_mode == Immediate)
            return get_8_bit_param();
        return mem[param];
    }

    void set_arithmetic_bits (std::int8_t val) 
    {
        reg.zero = val == 0;
        reg.negative = val < 0;
    }

    void set_comparsion_bits (std::int8_t val)
    {
        reg.carry = (val >= 0);
        reg.zero = (val == 0);
        reg.negative = (val < 0);
    }

    // TODO: CHEKC THIS SHIT 
    void ADC () 
    {
        std::int8_t M = get_M();
        bool carry = !((reg.A < 0) ^ (M < 0));
        std::int8_t tmp = reg.A + M;
        
        // If originally reg.A and M are both negative or both positive and the result is 
        // the opposite (positive/negative, respectively), overflow!
        carry = (carry && (M >> 7) != (tmp >> 7));
        reg.A = tmp + reg.carry;

        // If A+M is positive and the total sum A+M+C is negative, overflow!
        if(!(tmp >> 7) && (reg.A >> 7))
            carry = true;

        reg.carry = carry;
        reg.overflow = 
        set_arithmetic_bits(reg.A);
    }

    void AND ()
    {
        std::int8_t M = get_M();
        reg.A = reg.A & M;
        set_arithmetic_bits(reg.A);
    }

    void ASL () 
    {
        if(addr_mode == Accumulator)
        {
            reg.carry = reg.A & (1 << 7);
            reg.A = reg.A << 1;
            set_arithmetic_bits(reg.A);
        }
        else 
        {
            std::int8_t& M = mem[param];
            reg.carry = M & (1 << 7);
            M = M << 1;
            set_arithmetic_bits(M);
        }
    }

    void BCC ()
    {
        if(!reg.carry)
            reg.PC += param;
    }

    void BCS ()
    {
        if(reg.carry)
            reg.PC += param;
    }

    void BEQ ()
    {
        if(reg.zero)
            // TOOD: Convince James to let me add -1.
            reg.PC += param;
    }

    void BIT ()
    {
        std::uint8_t result = get_8_bit_param () & reg.A;
        reg.zero = (result == 0);

        reg.overflow = (1 << 6) & reg.A;
        reg.negative = (1 << 7) & reg.A;
    }

    void BMI ()
    {
        if(reg.negative)
            reg.PC += param;
    }

    void BNE ()
    {
        if(!reg.zero)
            reg.PC += param;
    }

    void BPL ()
    {
        if(!reg.negative)
            reg.PC += param;
    }

    void BRK ()
    {
        reg.break_command = 1;    
        std::int8_t bottom_bits = (std::int8_t)(reg.PC & 0xFF);
        std::int8_t top_bits = (std::int8_t)(reg.PC & 0xFF00);
        push_stack(bottom_bits);
        push_stack(top_bits);
        push_stack((std::int8_t)get_processor_status_byte());
        reg.PC = mem[0xFFFE] + (mem[0xFFFF] << 8);
    }

    void BVC ()
    {
        if(!reg.overflow)
            reg.PC += param;
    }

    void BVS ()
    {
        if(reg.overflow)
            reg.PC += param;
    }

    void CLC ()
    {
        reg.carry = false;
    }

    void CLD ()
    {
        reg.decimal_mode = false;
    }

    void CLI ()
    {
        reg.interrupt_disable = false;
    }

    void CLV ()
    {
        reg.overflow = false;
    }

    void DEC () { set_arithmetic_bits(mem[param] -= 1); }
    void DEX () { set_arithmetic_bits(reg.X -= 1); }
    void DEY () { set_arithmetic_bits(reg.Y -= 1); }
    void INC () { set_arithmetic_bits(mem[param] += 1); }
    void INX () { set_arithmetic_bits(reg.X += 1); }
    void INY () { set_arithmetic_bits(reg.Y += 1); }

    void CMP () { set_comparsion_bits(reg.A - get_M()); }
    void CPX () { set_comparsion_bits(reg.X - get_M()); }
    void CPY () { set_comparsion_bits(reg.Y - get_M()); }

    void JMP ()
    {
    	if(addr_mode == Indirect)
            reg.PC = mem[param] + (mem[param + 1] << 8);
        else
            reg.PC = param;
    }

    void JSR () 
    {
        std::int8_t bottom_bits = (std::int8_t)((reg.PC - 1) & 0xFF);
        std::int8_t top_bits = (std::int8_t)((reg.PC - 1) & 0xFF00);
        push_stack(bottom_bits);
        push_stack(top_bits);
        reg.PC = param;
    }

    void LDA () { set_arithmetic_bits(reg.A = mem[param]); }
    void LDX () { set_arithmetic_bits(reg.X = mem[param]); }
    void LDY () { set_arithmetic_bits(reg.Y = mem[param]); }
    void LSR () 
    {
        if(addr_mode == Accumulator)
        {
            reg.carry = reg.A & 1;
            reg.A = reg.A >> 1;
            set_arithmetic_bits(reg.A);
        }
        else 
        {
            std::int8_t& M = mem[param];
            reg.carry = M & 1;
            M = M >> 1;
            set_arithmetic_bits(M);
        }
    }

    void NOP () {}
    void ORA () { set_arithmetic_bits(reg.A = reg.A | get_M()); }
    void PHA () { push_stack(reg.A); }
    void PHP () { push_stack(get_processor_status_byte()); }
    void PLA () { reg.A = pop_stack(); } 
    void PLP () { set_processor_status_byte(pop_stack()); } 

    void ROL ()
    {
        if(addr_mode == Accumulator)
        {
            int prev_carry = reg.carry;
            reg.carry = reg.A & (1 << 7);
            reg.A = (reg.A << 1) + prev_carry;
            set_arithmetic_bits(reg.A);
        }
        else 
        {
            int prev_carry = reg.carry;
            std::int8_t& M = mem[param];
            reg.carry = M & (1 << 7);
            M = (M << 1) + prev_carry;
            set_arithmetic_bits(M);
        }
    }
    void ROR ()
    {
        if(addr_mode == Accumulator)
        {
            int prev_carry = reg.carry;
            reg.carry = reg.A & 1;
            reg.A = (reg.A >> 1) + (prev_carry << 7);
            set_arithmetic_bits(reg.A);
        }
        else 
        {
            int prev_carry = reg.carry;
            std::int8_t& M = mem[param];
            reg.carry = M & 1;
            M = (M >> 1) + (prev_carry << 7);
            set_arithmetic_bits(M);
        }
    }

    void RTI () 
    {
        set_processor_status_byte(pop_stack()); 
        reg.PC = (pop_stack() << 8);
        reg.PC += pop_stack();
    }

    void RTS () 
    {
        reg.PC = (pop_stack() << 8);
        reg.PC += pop_stack();
    }

    void SBC () 
    {
        // TODO 
    }

    void SEC () { reg.carry = true; } 
    void SED () { reg.decimal_mode = true; } 
    void SEI () { reg.interrupt_disable = true; } 

    void STA () { mem[param] = reg.A; } 
    void STX () { mem[param] = reg.X; } 
    void STY () { mem[param] = reg.Y; } 

    void TAX () { set_arithmetic_bits(reg.X = reg.A); } 
    void TAY () { set_arithmetic_bits(reg.Y = reg.A); } 
    void TSY () { set_arithmetic_bits(reg.Y = *stk); } 
    void TXA () { set_arithmetic_bits(reg.A = reg.X); } 
    void TXS () { set_arithmetic_bits(*stk = reg.X); } 
    void TYA () { set_arithmetic_bits(reg.A = reg.Y); } 

public:
    void main_loop (std::int8_t* nes, int N) 
    {
        int i = 0;
        while(i < N)
        {
            reg.PC++;
        }
    }
};
