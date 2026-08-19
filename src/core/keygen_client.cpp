#include "core/keygen_client.hpp"
#include "core/hex.hpp"

#include <Windows.h>

#include <sstream>
#include <stdexcept>
#include <vector>

namespace uds {
namespace {
std::wstring quoted_text(const std::wstring& text) {
  if (text.find(L'"') != std::wstring::npos) throw std::invalid_argument("quote in executable path");
  return L"\"" + text + L"\"";
}

std::wstring quoted(const std::filesystem::path& value) {
  return quoted_text(value.wstring());
}
}

std::vector<std::uint8_t> generate_key_x86(const std::filesystem::path& broker,
                                           const std::filesystem::path& dll,
                                           std::span<const std::uint8_t> seed,
                                           unsigned security_level,
                                           const std::wstring& variant) {
  if (!std::filesystem::is_regular_file(broker)) throw std::runtime_error("x86 keygen broker not found");
  if (!std::filesystem::is_regular_file(dll)) throw std::runtime_error("security DLL not found");
  std::wostringstream command;
  command << quoted(broker) << L' ' << quoted(dll) << L' ';
  const auto seed_hex = to_hex(seed, false);
  command << std::wstring(seed_hex.begin(), seed_hex.end()) << L" 0x" << std::hex << security_level
          << L' ' << quoted_text(variant);

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE read_pipe{};
  HANDLE write_pipe{};
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0) ||
      !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
    if (read_pipe) CloseHandle(read_pipe);
    if (write_pipe) CloseHandle(write_pipe);
    throw std::runtime_error("cannot create x86 keygen output pipe");
  }

  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  auto mutable_command = command.str();
  const auto created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  const auto create_error = GetLastError();
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    throw std::runtime_error("cannot start x86 keygen broker, Win32=" + std::to_string(create_error));
  }

  std::vector<char> buffer(512);
  std::string output;
  DWORD count{};
  while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0)
    output.append(buffer.data(), count);
  CloseHandle(read_pipe);
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code{};
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exit_code != 0) throw std::runtime_error("x86 keygen broker failed: " + output);
  auto key = from_hex(output);
  if (key.empty()) throw std::runtime_error("x86 keygen broker returned empty key");
  return key;
}

} // namespace uds
