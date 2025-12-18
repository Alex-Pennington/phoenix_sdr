# Phoenix SDR Unit Tests

## Overview

This directory contains unit tests for the Phoenix SDR project. Tests are written in C using a minimal test framework defined in `test_framework.h`.

## Running Tests

From the project root:

```powershell
# Run all tests
.\run_tests.ps1

# Run with verbose output
.\run_tests.ps1 -Verbose

# Run specific test (by filter)
.\run_tests.ps1 -Filter "tick"

# Build only (don't run)
.\run_tests.ps1 -BuildOnly
```

## Test Suites

| Test | Description | Module(s) Tested |
|------|-------------|------------------|
| `test_tcp_commands` | TCP command parser and executor | `src/tcp_commands.c` |
| `test_telemetry` | UDP telemetry broadcast | `tools/waterfall_telemetry.c` |
| `test_dsp` | DSP math (biquad, DC blocker) | Inline DSP functions |
| `test_mixer` | 450 kHz mixer math | Mixer algorithms |
| `test_tick_detector` | WWV tick pulse detection | `tools/tick_detector.c` |
| `test_marker_detector` | WWV minute marker detection | `tools/marker_detector.c` |
| `test_iq_recorder` | I/Q sample recording | `src/iq_recorder.c` |
| `test_wwv_gen` | WWV signal generator modules | `tools/oscillator.c`, `tools/bcd_encoder.c`, `tools/wwv_signal.c` |

## Test Framework

The test framework in `test_framework.h` provides:

### Test Definition
```c
TEST(my_test_name) {
    // test code
    PASS();
}
```

### Assertions
- `ASSERT(cond, msg)` - Basic assertion
- `ASSERT_EQ(a, b, msg)` - Integer/pointer equality
- `ASSERT_NE(a, b, msg)` - Integer/pointer inequality
- `ASSERT_GT(a, b, msg)` - Greater than
- `ASSERT_LT(a, b, msg)` - Less than
- `ASSERT_STR_EQ(a, b, msg)` - String equality
- `ASSERT_STR_CONTAINS(haystack, needle, msg)` - Substring check
- `ASSERT_FLOAT_EQ(a, b, eps, msg)` - Float equality within epsilon
- `ASSERT_NOT_NULL(ptr, msg)` - Pointer not NULL
- `ASSERT_NULL(ptr, msg)` - Pointer is NULL
- `ASSERT_TRUE(cond, msg)` - Boolean true
- `ASSERT_FALSE(cond, msg)` - Boolean false
- `PASS()` - Mark test as passed
- `FAIL(msg)` - Explicit failure

### Test Suite Structure
```c
int main(void) {
    TEST_BEGIN("My Test Suite");

    TEST_SECTION("Section Name");
    RUN_TEST(test_1);
    RUN_TEST(test_2);

    TEST_END();
    return TEST_EXIT_CODE();
}
```

## Adding New Tests

1. Create `test_<module>.c` in the `test/` directory
2. Include `test_framework.h`
3. Write tests using the macros above
4. Add the test to `$Tests` array in `run_tests.ps1`

Example entry in `run_tests.ps1`:
```powershell
@{
    Name = "test_my_module"
    Sources = @(
        "$TestDir\test_my_module.c",
        "$SrcDir\my_module.c"
    )
    Libs = @("-lm")
    Description = "My module tests"
}
```

## CI Integration

The tests are designed to return exit code 0 on success, non-zero on failure. This makes them suitable for CI pipelines.

```yaml
# Example GitHub Actions step
- name: Run Unit Tests
  run: .\run_tests.ps1
```

## Memory Testing

For debug builds, the framework includes basic memory tracking:
- `TRACK_ALLOC()` - Called when allocating
- `TRACK_FREE()` - Called when freeing
- `ASSERT_NO_LEAKS(msg)` - Verify alloc/free counts match
- `RESET_ALLOC_TRACKING()` - Reset counters

Note: For comprehensive leak detection, use Valgrind or AddressSanitizer in a proper CI environment.

---

## WWV Test Signal Generator

The `wwv_gen` tool generates reference WWV/WWVH time signals for testing decoder modules.

### Usage

```powershell
# Generate default 2-minute reference signal
.\bin\wwv_gen.exe -o wwv_test.iqr

# Generate signal starting at 12:30 on day 100 of 2025
.\bin\wwv_gen.exe -t 12:30 -d 100 -y 25 -s wwv -o test.iqr

# Generate WWVH signal (1200 Hz tick instead of 1000 Hz)
.\bin\wwv_gen.exe -s wwvh -o wwvh_test.iqr
```

### Reference Signals

Generated signals follow the WWV Test Signal Specification (see `docs/WWV_Test_Signal_Generator_Specification.md`):

- **Sample rate:** 2 Msps (2,000,000 samples/second)
- **Duration:** Fixed 2 minutes (240M samples, ~915 MB file)
- **Format:** `.iqr` (int16_t interleaved I/Q pairs)
- **Timing:** Sample-accurate per NIST spec (500 ns resolution)
- **BCD modulation:** Exact 18% high / 3% low levels
- **Phase coherence:** Maintained across guard zones

### Testing with Generated Signals

```powershell
# Generate test signal
.\bin\wwv_gen.exe -t 00:00 -d 001 -y 25 -o test_signal.iqr

# Play back through waterfall for verification
.\bin\iqr_play.exe test_signal.iqr | .\bin\waterfall.exe

# Run automated tests
.\bin\test_wwv_gen.exe
```

### Test Coverage

The `test_wwv_gen` suite validates:

1. **Oscillator Module**
   - Phase continuity across 1000+ samples
   - Amplitude control accuracy

2. **BCD Encoder**
   - Pulse width timing (200ms/500ms/800ms)
   - Modulation level accuracy (0.18 ± 0.001, 0.03 ± 0.001)
   - Frame structure per NIST spec

3. **Signal Timing**
   - Tick pulse at samples 20,000-30,000
   - Marker pulse duration (1.6M samples = 800ms)
   - Guard zone silence
   - Tone schedule (500/600/440 Hz per minute)
