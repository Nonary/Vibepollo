/**
 * Measured image-area widths offered as suggestions for `dd_virtual_display_image_width_mm`.
 *
 * That setting takes the width, in millimetres, of the image area a client actually displays.
 * Working it out means reading a datasheet, so the devices someone has already worked out are
 * kept here rather than measured twice. This is an ordinary list with no behaviour attached:
 * add a row to offer another device, delete one to stop offering it. Nothing else keys off the
 * labels, and any width can still be typed by hand, so a device missing from the list is a
 * convenience gap and never a blocker.
 *
 * `widthMm` is the full width of the panel's active area, rounded to whole millimetres because
 * that is the granularity an EDID carries. A stream wider in aspect than the panel is
 * letterboxed and still fills that width; a stream narrower than the panel is pillarboxed and
 * needs a smaller number than the one listed here.
 */
export interface ClientImageWidthPreset {
  /** Device the measurement belongs to. A product name, so it is not translated. */
  label: string;
  /** Pixel mode the measurement was taken at, shown for context. */
  mode: string;
  /** Width of the panel's active image area, in whole millimetres. */
  widthMm: number;
}

export const clientImageWidthPresets: ClientImageWidthPreset[] = [
  // Dell's manual gives a 344.68 x 215.42 mm active area at 2560x1600, so 188.65 PPI.
  { label: 'Alienware m16 R2', mode: '2560x1600', widthMm: 345 },
  // Samsung's 8.0-inch inner panel at 2504x2256 works out to 150.97 x 136.01 mm, so 421.3 PPI.
  { label: 'Galaxy Z Fold8 Ultra (inner panel)', mode: '2504x2256', widthMm: 151 },
];
