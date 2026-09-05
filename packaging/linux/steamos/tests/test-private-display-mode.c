/* SPDX-License-Identifier: GPL-3.0-only */
/* Compile against the copied, patched kernel module's portable mode policy. */
#include <assert.h>
#include <stdio.h>
#include <vibeshine_drm_mode.h>

static void check_mode(unsigned width, unsigned height, unsigned millihz,
                       unsigned expected_clock_khz)
{
  const struct vibeshine_drm_requested_mode mode = {width, height, millihz};
  assert(vibeshine_drm_requested_mode_valid(&mode));
  const unsigned clock = vibeshine_drm_requested_mode_clock_khz(&mode);
  assert(clock == expected_clock_khz);
  const double actual_millihz = (double)clock * 1000000.0 /
                               ((double)(width + 160) * (height + 45));
  double error = actual_millihz - millihz;
  if (error < 0) error = -error;
  assert(error < 200.0); /* Existing host policy requires less than 0.2 Hz. */
  printf("%ux%u requested=%u mHz actual=%.6f mHz\n", width, height, millihz, actual_millihz);
}

int main(void)
{
  check_mode(3024, 1890, 120000, 739325);
  check_mode(3033, 1891, 119880, 741056);
  check_mode(1920, 1080, 59940, 140260);
  check_mode(64, 64, 1000, 24);
  check_mode(8192, 8192, 1000000, 68795424);
  const struct vibeshine_drm_requested_mode invalid[] = {
    {0, 0, 0}, {63, 1080, 60000}, {1920, 63, 60000},
    {8193, 1080, 60000}, {1920, 8193, 60000},
    {1920, 1080, 999}, {1920, 1080, 1000001},
    {~0u, 1080, 60000}, {1920, ~0u, 60000}, {1920, 1080, ~0u},
  };
  for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    assert(!vibeshine_drm_requested_mode_valid(&invalid[i]));
  return 0;
}
