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
