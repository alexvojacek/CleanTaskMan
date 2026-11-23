#pragma once
#include <windows.h>
#include <map>
#include <span>
#include <vector>
namespace cpu_parking {
using Key = std::pair<unsigned, unsigned>;
using States = std::map<Key, bool>;
States parse(std::span<const unsigned char> bytes);
class Sampler {
  public:
    States capture();

  private:
    std::vector<unsigned char> buffer;
};
} // namespace cpu_parking
