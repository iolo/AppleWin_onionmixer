/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2024, Tom Charlesworth, Michael Pohoreski

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "StdAfx.h"

#include "DebugStreamProvider.h"

// AppleWin includes
#include "Common.h"
#include "Core.h"
#include "CPU.h"
#include "Memory.h"

#include <sstream>
#include <iomanip>
#include <chrono>

namespace debugserver {

DebugStreamProvider::DebugStreamProvider() {
}

//-----------------------------------------------------------------------------
// System Messages
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetHelloMessage() {
    std::map<std::string, std::string> extra;
    extra["ver"] = VERSION;
    extra["ts"] = std::to_string(GetTimestamp());
    return FormatLine("sys", "conn", "hello", "AppleWin Debug Stream", extra);
}

std::string DebugStreamProvider::GetGoodbyeMessage() {
    std::map<std::string, std::string> extra;
    extra["ts"] = std::to_string(GetTimestamp());
    return FormatLine("sys", "conn", "goodbye", "", extra);
}

std::string DebugStreamProvider::GetErrorMessage(const std::string& error) {
    return FormatLine("sys", "error", "msg", error);
}

//-----------------------------------------------------------------------------
// CPU Information
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetCPURegisters() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string result;

    result += FormatLine("cpu", "reg", "a", ToHex8(regs.a)) + "\r\n";
    result += FormatLine("cpu", "reg", "x", ToHex8(regs.x)) + "\r\n";
    result += FormatLine("cpu", "reg", "y", ToHex8(regs.y)) + "\r\n";
    result += FormatLine("cpu", "reg", "pc", ToHex16(regs.pc)) + "\r\n";
    result += FormatLine("cpu", "reg", "sp", ToHex8(static_cast<uint8_t>(regs.sp & 0xFF))) + "\r\n";
    result += FormatLine("cpu", "reg", "p", ToHex8(regs.ps));

    return result;
}

std::string DebugStreamProvider::GetCPURegister(const std::string& regName) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (regName == "a") {
        return FormatLine("cpu", "reg", "a", ToHex8(regs.a));
    } else if (regName == "x") {
        return FormatLine("cpu", "reg", "x", ToHex8(regs.x));
    } else if (regName == "y") {
        return FormatLine("cpu", "reg", "y", ToHex8(regs.y));
    } else if (regName == "pc") {
        return FormatLine("cpu", "reg", "pc", ToHex16(regs.pc));
    } else if (regName == "sp") {
        return FormatLine("cpu", "reg", "sp", ToHex8(static_cast<uint8_t>(regs.sp & 0xFF)));
    } else if (regName == "p") {
        return FormatLine("cpu", "reg", "p", ToHex8(regs.ps));
    }

    return "";
}

std::string DebugStreamProvider::GetCPUFlags() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string result;

    uint8_t ps = regs.ps;

    result += FormatLine("cpu", "flag", "n", (ps & AF_SIGN) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "v", (ps & AF_OVERFLOW) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "b", (ps & AF_BREAK) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "d", (ps & AF_DECIMAL) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "i", (ps & AF_INTERRUPT) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "z", (ps & AF_ZERO) ? "1" : "0") + "\r\n";
    result += FormatLine("cpu", "flag", "c", (ps & AF_CARRY) ? "1" : "0");

    return result;
}

std::string DebugStreamProvider::GetCPUState() {
    std::lock_guard<std::mutex> lock(m_mutex);

    return FormatLine("cpu", "state", "jammed", regs.bJammed ? "1" : "0");
}

//-----------------------------------------------------------------------------
// Memory Information
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetMemoryRead(uint16_t addr, uint8_t value) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    return FormatLine("mem", "read", "byte", ToHex8(value), extra);
}

std::string DebugStreamProvider::GetMemoryWrite(uint16_t addr, uint8_t value) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    return FormatLine("mem", "write", "byte", ToHex8(value), extra);
}

std::string DebugStreamProvider::GetMemoryDump(uint16_t startAddr, const std::vector<uint8_t>& data) {
    std::string result;
    for (size_t i = 0; i < data.size(); ++i) {
        std::map<std::string, std::string> extra;
        extra["addr"] = ToHex16(static_cast<uint16_t>(startAddr + i));
        result += FormatLine("mem", "dump", "byte", ToHex8(data[i]), extra);
        if (i < data.size() - 1) {
            result += "\r\n";
        }
    }
    return result;
}

std::string DebugStreamProvider::GetMemoryBankStatus() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string result;

    UINT memMode = GetMemMode();

    // Memory mode flags
    result += FormatLine("mem", "bank", "mode", ToHex8(static_cast<uint8_t>(memMode & 0xFF)));

    return result;
}

//-----------------------------------------------------------------------------
// I/O Information
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetSoftSwitchRead(uint16_t addr, uint8_t value) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    return FormatLine("io", "sw_read", "val", ToHex8(value), extra);
}

std::string DebugStreamProvider::GetSoftSwitchWrite(uint16_t addr, uint8_t value) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    return FormatLine("io", "sw_write", "val", ToHex8(value), extra);
}

//-----------------------------------------------------------------------------
// Machine Information
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetMachineInfo() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string result;

    // Machine type
    const char* machineType = "Unknown";
    switch (GetApple2Type()) {
        case A2TYPE_APPLE2:      machineType = "Apple2"; break;
        case A2TYPE_APPLE2PLUS:  machineType = "Apple2Plus"; break;
        case A2TYPE_APPLE2JPLUS: machineType = "Apple2JPlus"; break;
        case A2TYPE_APPLE2E:     machineType = "Apple2e"; break;
        case A2TYPE_APPLE2EENHANCED: machineType = "Apple2eEnhanced"; break;
        default: break;
    }

    result += FormatLine("mach", "info", "type", machineType);

    return result;
}

std::string DebugStreamProvider::GetMachineStatus(const std::string& mode) {
    return FormatLine("mach", "status", "mode", mode);
}

//-----------------------------------------------------------------------------
// Debug Information
//-----------------------------------------------------------------------------

std::string DebugStreamProvider::GetBreakpointHit(int index, uint16_t addr) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    extra["idx"] = std::to_string(index);
    return FormatLine("dbg", "bp", "hit", "1", extra);
}

std::string DebugStreamProvider::GetTraceExec(uint16_t addr, const std::string& disasm) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    return FormatLine("dbg", "trace", "exec", EscapeJson(disasm), extra);
}

std::string DebugStreamProvider::GetTraceMemory(uint16_t addr, uint8_t value, bool isWrite) {
    std::map<std::string, std::string> extra;
    extra["addr"] = ToHex16(addr);
    extra["rw"] = isWrite ? "w" : "r";
    return FormatLine("dbg", "trace", "mem", ToHex8(value), extra);
}

//-----------------------------------------------------------------------------
// Full State Snapshot
//-----------------------------------------------------------------------------

std::vector<std::string> DebugStreamProvider::GetFullSnapshot() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> lines;

    // ===== Machine info =====
    const char* machineType = "Unknown";
    switch (GetApple2Type()) {
        case A2TYPE_APPLE2:      machineType = "Apple2"; break;
        case A2TYPE_APPLE2PLUS:  machineType = "Apple2Plus"; break;
        case A2TYPE_APPLE2JPLUS: machineType = "Apple2JPlus"; break;
        case A2TYPE_APPLE2E:     machineType = "Apple2e"; break;
        case A2TYPE_APPLE2EENHANCED: machineType = "Apple2eEnhanced"; break;
        case A2TYPE_APPLE2C:     machineType = "Apple2c"; break;
        case A2TYPE_PRAVETS82:   machineType = "Pravets82"; break;
        case A2TYPE_PRAVETS8M:   machineType = "Pravets8M"; break;
        case A2TYPE_PRAVETS8A:   machineType = "Pravets8A"; break;
        case A2TYPE_TK30002E:    machineType = "TK30002e"; break;
        case A2TYPE_BASE64A:     machineType = "Base64A"; break;
        default: break;
    }
    lines.push_back(FormatLine("mach", "info", "type", machineType));

    // ===== CPU type (NEW - from 65501) =====
    const char* cpuType = "Unknown";
    switch (GetMainCpu()) {
        case CPU_6502:  cpuType = "6502"; break;
        case CPU_65C02: cpuType = "65C02"; break;
        case CPU_Z80:   cpuType = "Z80"; break;
        default: break;
    }
    lines.push_back(FormatLine("mach", "info", "cpuType", cpuType));

    // ===== Memory mode (get early for videoMode calculation) =====
    UINT memMode = GetMemMode();

    // ===== Video mode (NEW - from 65501) =====
    const char* videoMode = "Unknown";
    if (memMode & MF_HIRES) {
        videoMode = (memMode & MF_80STORE) ? "DoubleHiRes" : "HiRes";
    } else {
        videoMode = (memMode & MF_80STORE) ? "80ColText" : "TextLoRes";
    }
    lines.push_back(FormatLine("mach", "info", "videoMode", videoMode));

    // ===== Machine status =====
    const char* mode = "unknown";
    switch (g_nAppMode) {
        case MODE_LOGO:      mode = "logo"; break;
        case MODE_RUNNING:   mode = "running"; break;
        case MODE_DEBUG:     mode = "debug"; break;
        case MODE_STEPPING:  mode = "stepping"; break;
        case MODE_PAUSED:    mode = "paused"; break;
        case MODE_BENCHMARK: mode = "benchmark"; break;
        default: break;
    }
    lines.push_back(FormatLine("mach", "status", "mode", mode));

    // ===== Cumulative cycles (NEW - from 65501) =====
    lines.push_back(FormatLine("mach", "info", "cycles", std::to_string(g_nCumulativeCycles)));

    // ===== CPU registers =====
    lines.push_back(FormatLine("cpu", "reg", "a", ToHex8(regs.a)));
    lines.push_back(FormatLine("cpu", "reg", "x", ToHex8(regs.x)));
    lines.push_back(FormatLine("cpu", "reg", "y", ToHex8(regs.y)));
    lines.push_back(FormatLine("cpu", "reg", "pc", ToHex16(regs.pc)));
    lines.push_back(FormatLine("cpu", "reg", "sp", ToHex8(static_cast<uint8_t>(regs.sp & 0xFF))));
    lines.push_back(FormatLine("cpu", "reg", "p", ToHex8(regs.ps)));

    // ===== CPU flags =====
    uint8_t ps = regs.ps;
    lines.push_back(FormatLine("cpu", "flag", "n", (ps & AF_SIGN) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "v", (ps & AF_OVERFLOW) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "b", (ps & AF_BREAK) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "d", (ps & AF_DECIMAL) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "i", (ps & AF_INTERRUPT) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "z", (ps & AF_ZERO) ? "1" : "0"));
    lines.push_back(FormatLine("cpu", "flag", "c", (ps & AF_CARRY) ? "1" : "0"));

    // ===== CPU state =====
    lines.push_back(FormatLine("cpu", "state", "jammed", regs.bJammed ? "1" : "0"));

    // ===== Memory bank mode =====
    lines.push_back(FormatLine("mem", "bank", "mode", ToHex8(static_cast<uint8_t>(memMode & 0xFF))));

    // ===== Memory flags (NEW - from 65501) =====
    lines.push_back(FormatLine("mem", "flag", "80store", (memMode & MF_80STORE) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "auxRead", (memMode & MF_AUXREAD) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "auxWrite", (memMode & MF_AUXWRITE) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "altZP", (memMode & MF_ALTZP) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "highRam", (memMode & MF_HIGHRAM) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "bank2", (memMode & MF_BANK2) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "writeRam", (memMode & MF_WRITERAM) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "page2", (memMode & MF_PAGE2) ? "1" : "0"));
    lines.push_back(FormatLine("mem", "flag", "hires", (memMode & MF_HIRES) ? "1" : "0"));

    return lines;
}

//-----------------------------------------------------------------------------
// Utility Methods
//-----------------------------------------------------------------------------

int64_t DebugStreamProvider::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

std::string DebugStreamProvider::FormatLine(const char* cat, const char* sec, const char* fld,
                                            const std::string& val,
                                            const std::map<std::string, std::string>& extra) {
    std::ostringstream json;

    json << "{\"emu\":\"apple\""
         << ",\"cat\":\"" << cat << "\""
         << ",\"sec\":\"" << sec << "\""
         << ",\"fld\":\"" << fld << "\""
         << ",\"val\":\"" << val << "\"";

    for (const auto& kv : extra) {
        json << ",\"" << kv.first << "\":\"" << kv.second << "\"";
    }

    json << "}";
    return json.str();
}

std::string DebugStreamProvider::ToHex8(uint8_t value) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<int>(value);
    return oss.str();
}

std::string DebugStreamProvider::ToHex16(uint16_t value) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
        << static_cast<int>(value);
    return oss.str();
}

std::string DebugStreamProvider::EscapeJson(const std::string& str) {
    std::string result;
    result.reserve(str.size() + 16);

    for (char c : str) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b";  break;
            case '\f': result += "\\f";  break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control character - escape as \u00XX
                    std::ostringstream oss;
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c));
                    result += oss.str();
                } else {
                    result += c;
                }
                break;
        }
    }

    return result;
}

} // namespace debugserver
