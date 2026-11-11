# Host e-paper regression tests

Run from the repository root:

```sh
tests/host/run.sh
ASAN_OPTIONS=detect_leaks=0 SANITIZE=1 tests/host/run.sh
```

The runner uses host GCC/Clang, compiles the **actual** shared panel layer and
SSD1677, SSD2677, UC8179 drivers, and retains its executables in a temporary directory.
No ESP-IDF checkout or panel hardware is required. The small stubs replace only
ESP-IDF GPIO, SPI, time, semaphores and capability-based allocation.

The runner first checks that the runtime firmware version and ESP-IDF image descriptor agree. Update `platformio.ini` and `CMakeLists.txt` together for a new release.

Tests capture command/data SPI traffic and check:

- Board SPI configuration: GPIO14 bidirectional SDA, GPIO12 SD MISO,
  half-duplex 3-wire mode without dummy cycles, and a 2 MHz clock.

- AUTO ID 0x07 selection, 0x00/0xff/0x01 legacy fallback, read error propagation,
  explicit model selection without probing, and constructor cleanup.
- Opposite BUSY polarity, completion edge, timeout and active-wait cleanup.
- Every pixel of 96,000-byte SSD2677 full mono and gray4 streams, with and without
  horizontal mirroring.
- Gray4 followed by a caller-supplied mono partial baseline, unchanged pixels
  and changed pixels in the complete previous/current stream, explicit diff
  input, and previous-frame synchronization after commit.
- Analog power retained for consecutive partials, idle power-off idempotence,
  deep-sleep key, and wakeup.
- SSD1677 full mono planes, partial previous/current register routing, both
  gray4 planes, and custom post-gray4 partial baseline.
- Capability-allocation failure rollback and SPI prepare/commit failures.
- SSD2677 temperature waveform register 0xE6 at raw 10/11, 20/21, 30/31,
  127/128 for mono full/partial and 15/16, 127/128 for gray4. Temperature
  read failure preserves the complete previous baseline, does not stream or
  trigger refresh, and a fresh full initialization/refresh recovers.

AddressSanitizer and UndefinedBehaviorSanitizer are enabled with `SANITIZE=1`.
LeakSanitizer is disabled in the example because it cannot run under the
execution environment's ptrace wrapper. Allocation/semaphore counters still
assert that each tested panel lifecycle and failed constructor releases its
mocked resources. The plain libc panel allocation is checked by ASan but not
injected. Hardware waveform quality, actual BUSY timing/ISR scheduling, SPI
half-duplex electrical behavior, and controller temperature readings still
require an on-device check.
