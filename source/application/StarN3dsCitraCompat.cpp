#ifdef STAR_PLATFORM_N3DS

// Citra Nightly 2104 does not implement PTMSYSM ConfigureNew3DSCPU.
// Return success here so callers don't propagate an emulator-specific failure.
extern "C" int __wrap_PTMSYSM_ConfigureNew3DSCPU(unsigned char value) {
  (void)value;
  return 0;
}

// Some libctru call paths adjust app CPU time limits during startup.
// Keep this as a no-op success on emulator builds for stability.
extern "C" int __wrap_APT_SetAppCpuTimeLimit(unsigned int percent) {
  (void)percent;
  return 0;
}

#endif
