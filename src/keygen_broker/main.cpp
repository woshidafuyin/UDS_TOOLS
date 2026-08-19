#include <Windows.h>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::vector<std::uint8_t> parse_hex(std::wstring text) {
  std::wstring clean;
  for (auto ch : text) if (iswxdigit(ch)) clean.push_back(ch);
  if (clean.size() % 2 != 0) throw std::runtime_error("invalid seed hex");
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i < clean.size(); i += 2)
    out.push_back(static_cast<std::uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16)));
  return out;
}
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::cerr << "usage: keygen_broker.exe <dll> <seed_hex> [level] [variant]\n";
    return 2;
  }
  try {
    const auto seed = parse_hex(argv[2]);
    const auto level = argc >= 4 ? static_cast<unsigned>(std::stoul(argv[3], nullptr, 0)) : 0x11U;
    std::string variant = "chuneng";
    if (argc >= 5) {
      const auto chars = static_cast<int>(wcslen(argv[4]));
      const auto len = WideCharToMultiByte(CP_UTF8, 0, argv[4], chars, nullptr, 0, nullptr, nullptr);
      variant.resize(static_cast<std::size_t>(len));
      WideCharToMultiByte(CP_UTF8, 0, argv[4], chars, variant.data(), len, nullptr, nullptr);
    }
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) {
      const auto error = GetLastError();
      std::ostringstream message;
      message << "cannot load OEM DLL, Win32=" << error;
      if (error == ERROR_BAD_EXE_FORMAT) {
        message << " (architecture mismatch: OEM SeedKey DLL requires a 32-bit broker)";
      }
      throw std::runtime_error(message.str());
    }
    // Vector standard GenerateKeyEx uses cdecl. A wrong calling convention
    // corrupts the x86 stack after return.
    using GenerateKeyEx = int(__cdecl*)(const unsigned char*, unsigned, unsigned, const char*,
                                          unsigned char*, unsigned, unsigned*);
    auto fn = reinterpret_cast<GenerateKeyEx>(GetProcAddress(dll, "GenerateKeyEx"));
    if (!fn) { FreeLibrary(dll); throw std::runtime_error("GenerateKeyEx export not found"); }
    std::vector<std::uint8_t> key(64);
    unsigned actual = 0;
    const auto rc = fn(seed.data(), static_cast<unsigned>(seed.size()), level, variant.c_str(),
                       key.data(), static_cast<unsigned>(key.size()), &actual);
    FreeLibrary(dll);
    if (rc != 0 || actual == 0 || actual > key.size()) throw std::runtime_error("GenerateKeyEx failed");
    key.resize(actual);
    std::cout << std::uppercase << std::hex << std::setfill('0');
    for (auto b : key) std::cout << std::setw(2) << static_cast<unsigned>(b);
    std::cout << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << '\n';
    return 1;
  }
}
