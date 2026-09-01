#include "util.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>

namespace irez {

std::string uuid4() {
  std::array<std::uint8_t, 16> bytes{};
  std::random_device rd;
  // Fill 4 bytes at a time; random_device::result_type is unsigned int.
  for (std::size_t i = 0; i < bytes.size(); i += sizeof(unsigned int)) {
    unsigned int value = rd();
    for (std::size_t j = 0; j < sizeof(unsigned int) && i + j < bytes.size(); ++j)
      bytes[i + j] = static_cast<std::uint8_t>(value >> (8 * j));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0F) | 0x40); // version 4
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3F) | 0x80); // variant 1
  char text[37];
  std::snprintf(text, sizeof(text),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
                bytes[13], bytes[14], bytes[15]);
  return text;
}

std::string now_iso8601() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto micros = duration_cast<microseconds>(now.time_since_epoch()) % seconds(1);
  const std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  // Render the fraction digit by digit: always exactly six digits, no
  // snprintf format-truncation surprises.
  char fraction[7];
  unsigned value = static_cast<unsigned>(micros.count());
  for (int i = 5; i >= 0; --i) {
    fraction[i] = static_cast<char>('0' + value % 10);
    value /= 10;
  }
  fraction[6] = '\0';
  std::ostringstream text;
  text << std::setfill('0') << std::setw(4) << tm.tm_year + 1900 << '-'
       << std::setw(2) << tm.tm_mon + 1 << '-' << std::setw(2) << tm.tm_mday
       << 'T' << std::setw(2) << tm.tm_hour << ':' << std::setw(2) << tm.tm_min
       << ':' << std::setw(2) << tm.tm_sec << '.' << fraction << "+00:00";
  return text.str();
}

std::string lower_suffix(const std::string &path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

} // namespace irez
