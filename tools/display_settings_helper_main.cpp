/**
 * @file tools/display_settings_helper_main.cpp
 * @brief Entry point for the v1 Sunshine display settings helper.
 */
#ifdef _WIN32

int run_legacy_helper(int argc, char *argv[]);

int main(int argc, char *argv[]) {
  return run_legacy_helper(argc, argv);
}

#else
int main() {
  return 0;
}
#endif
