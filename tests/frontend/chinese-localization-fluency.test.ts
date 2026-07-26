import zh from '@web/public/assets/locale/zh.json';
import zhTW from '@web/public/assets/locale/zh_TW.json';
import { getConfigSelectOptions } from '@web/configs/configSelectOptions';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const captureSource = readFileSync(resolve(process.cwd(), 'configs/tabs/Capture.vue'), 'utf8');
const displayOptionsSource = readFileSync(
  resolve(process.cwd(), 'configs/tabs/audiovideo/DisplayDeviceOptions.vue'),
  'utf8',
);
const appEditModalSource = readFileSync(
  resolve(process.cwd(), 'components/AppEditModal.vue'),
  'utf8',
);
const checkboxSource = readFileSync(resolve(process.cwd(), 'Checkbox.vue'), 'utf8');
const appEditConfigOverridesSource = readFileSync(
  resolve(process.cwd(), 'components/app-edit/AppEditConfigOverridesSection.vue'),
  'utf8',
);
const appEditLosslessSource = readFileSync(
  resolve(process.cwd(), 'components/app-edit/AppEditLosslessScalingSection.vue'),
  'utf8',
);
const appEditBasicsSource = readFileSync(
  resolve(process.cwd(), 'components/app-edit/AppEditBasicsSection.vue'),
  'utf8',
);
const appEditRtxHdrSource = readFileSync(
  resolve(process.cwd(), 'components/app-edit/AppEditRtxHdrSection.vue'),
  'utf8',
);

describe('Chinese localization fluency', () => {
  it('keeps Simplified Chinese high-traffic labels natural and consistent', () => {
    expect(zh.config.global_prep_cmd).toBe('命令准备工作');
    expect(zh.index.web_links).toBe('网页链接');
    expect(zh.changelog.filter_line).toBe('当前版本系列');
    expect(zh.changelog.release_line_title).toBe('版本系列 {line}');
    expect(zh.troubleshooting.logs_desc).toBe('查看 Vibepollo 上传的日志');
    expect(zh.stats.config_desc).toBe(
      '控制 Sunshine 如何收集和显示实时主机统计、图表和会话历史记录。这些设置与“统计”页面共用。',
    );
  });

  it('keeps Traditional Chinese Taiwan UI terms and punctuation consistent', () => {
    expect(zhTW.config.dd_snapshot_restore_hotkey).toBe('還原快照快速鍵');
    expect(zhTW.config.dd_snapshot_restore_hotkey_reset).toBe('清除快速鍵');
    expect(zhTW.config.dd_snapshot_restore_hotkey_desc).toContain('全域快速鍵');
    expect(zhTW.config.dd_display_helper_engine_auto).toContain('預先發行版本');
    expect(zhTW.clients.search_placeholder).toBe('名稱、ID、顯示器、HDR…');
    expect(zhTW.config.update_check_interval_desc).toContain('依據 Git 標籤');
    expect(zhTW.changelog.filter_line).toBe('目前版本系列');
    expect(zhTW.changelog.release_line_title).toBe('版本系列 {line}');
    expect(zhTW.apps.applications_intro).toContain('手動新增應用程式');
    expect(zhTW.stats.config_desc).toBe(
      '控制 Sunshine 收集及顯示即時主機統計資料、圖表和工作階段歷程的方式。這些設定與「統計」頁面共用。',
    );
  });

  it('uses a natural Traditional Chinese locale option label', () => {
    const options = getConfigSelectOptions('locale', {
      t: (key) => key,
      platform: 'windows',
      currentValue: 'zh_TW',
    });

    expect(options.find((option) => option.value === 'zh_TW')?.label).toBe('繁體中文');
  });

  it('moves visible English settings copy behind locale keys', () => {
    expect(captureSource).not.toContain('Lossless Scaling status unavailable.');
    expect(captureSource).not.toContain('Override Path');
    expect(captureSource).not.toContain('Hide Override');
    expect(captureSource).not.toContain('Using:');
    expect(captureSource).not.toContain('Enable Lossless Scaling per application');
    expect(captureSource).not.toContain('Lossless Scaling executable');
    expect(captureSource).not.toContain('Select Lossless Scaling Executable');
    expect(captureSource).not.toContain('Detected installations');
    expect(captureSource).not.toContain('Use Selected Path');
    expect(captureSource).not.toContain('Use Suggested');
    expect(captureSource).not.toContain('Default installation:');
    expect(captureSource).not.toContain('Vibeshine could not find Lossless Scaling');
    expect(captureSource).not.toContain('Vibeshine searched common Steam');
    expect(captureSource).toContain("t('config.lossless.status_unavailable')");
    expect(captureSource).toContain("$t('config.lossless.app_hint')");
    expect(appEditLosslessSource).not.toContain('Upscaling Filter');
    expect(appEditLosslessSource).not.toContain('Filter used after downscaling');
    expect(appEditLosslessSource).not.toContain('Resolution Scale');
    expect(appEditLosslessSource).not.toContain('Performance Note:');
    expect(appEditLosslessSource).not.toContain('Only use upscaling if');
    expect(appEditLosslessSource).not.toContain('Sharpening (1-10)');
    expect(appEditLosslessSource).not.toContain('Anime4K Size');
    expect(appEditLosslessSource).not.toContain('Enable Variable Rate Shading');
    expect(appEditLosslessSource).toContain("$t('apps.framegen.upscaling_filter')");
    expect(displayOptionsSource).not.toContain('Overrides below are disabled');
    expect(displayOptionsSource).toContain("$t('config.dd_manual_enforcement_notice')");
  });

  it('does not pass already translated checkbox labels back through i18n lookup', () => {
    expect(checkboxSource).not.toContain('$t(labelField)');
    expect(checkboxSource).not.toContain('$t(descField)');
  });

  it('guards optional app override description translation lookups', () => {
    const descStart = appEditConfigOverridesSource.indexOf('function descFor(key: string)');
    const descEnd = appEditConfigOverridesSource.indexOf('\n}', descStart);
    const descSource = appEditConfigOverridesSource.slice(descStart, descEnd);

    expect(appEditConfigOverridesSource).toContain('const i18n = useI18n();');
    expect(appEditConfigOverridesSource).toContain(
      'const te: typeof i18n.te = i18n.te.bind(i18n);',
    );
    expect(descStart).toBeGreaterThanOrEqual(0);
    expect(descSource).toContain("if (!te(k)) return '';");
    expect(descSource.indexOf('if (!te(k))')).toBeLessThan(descSource.indexOf('const v = t(k);'));
  });

  it('localizes app edit modal source copy shown during browser review', () => {
    const sources = [
      appEditModalSource,
      appEditBasicsSource,
      appEditRtxHdrSource,
      appEditConfigOverridesSource,
    ];
    const hardcodedEnglish = [
      'Edit Application',
      'Exclude Global Prep',
      'Linked to Playnite',
      'Enable RTX HDR conversion for this app.',
      'Setting Overrides',
      'Override global settings for this application only.',
    ];

    for (const source of sources) {
      for (const phrase of hardcodedEnglish) {
        expect(source).not.toContain(phrase);
      }
    }
  });

  it('initializes the RTX HDR live suppress flag before the immediate watcher uses it', () => {
    const declarationIndex = appEditModalSource.indexOf('let liveRtxHdrSuppress = false;');
    const watcherIndex = appEditModalSource.indexOf('watch(\n  () => props.app,');

    expect(declarationIndex).toBeGreaterThanOrEqual(0);
    expect(watcherIndex).toBeGreaterThanOrEqual(0);
    expect(declarationIndex).toBeLessThan(watcherIndex);
  });
});
