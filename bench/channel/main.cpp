#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>
#include <oran/channel/adapter.hpp>
int main() {
  const std::string text(16000, 'x');
  ankerl::nanobench::Bench{}.run("channel text splitting", [&] {
    ankerl::nanobench::doNotOptimizeAway(orangutan::channel::split_text(text, 4000));
  });
}
