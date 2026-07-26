<template>
  <div v-if="sessions.length > 0" class="mb-4">
    <div class="flex flex-wrap items-center gap-2 mb-3">
      <n-tag type="info" size="small" :bordered="false">RTSP</n-tag>
      <span class="text-sm font-medium">
        {{ t('sessions.rtsp_active', { count: sessions.length }) }}
      </span>
      <n-tag v-if="appRunning" type="success" size="small" :bordered="false" class="max-w-full">
        <span class="inline-flex min-w-0 max-w-full items-center">
          <i class="fas fa-gamepad mr-1 flex-shrink-0" />
          <span class="truncate">{{ appName || t('sessions.app_running') }}</span>
        </span>
      </n-tag>
    </div>

    <div class="space-y-3">
      <div
        v-for="session in sessions"
        :key="session.uuid"
        class="rounded-xl border border-dark/[0.06] bg-light/[0.03] p-4 dark:border-light/[0.10] dark:bg-dark/[0.06]"
      >
        <div class="flex flex-wrap items-center gap-2 mb-3">
          <span class="text-sm font-semibold">{{
            session.device_name || session.uuid.substring(0, 8)
          }}</span>
          <n-tag type="success" size="small" :bordered="false">
            <span class="inline-flex items-center"
              ><i class="fas fa-video mr-1" />{{ t('sessions.video') }}</span
            >
          </n-tag>
          <n-tag v-if="session.hdr" type="warning" size="small" :bordered="false">HDR</n-tag>
          <n-tag v-if="session.yuv444" type="info" size="small" :bordered="false">YUV444</n-tag>
          <n-tag type="default" size="small" :bordered="false">{{ session.state }}</n-tag>
        </div>

        <div class="grid grid-cols-1 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-6 gap-3">
          <StatCell
            v-if="session.width && session.height"
            :label="t('sessions.resolution')"
            :value="`${session.width}×${session.height}`"
            :tip="t('sessions.tip_resolution')"
          />
          <StatCell
            v-if="session.fps"
            :label="t('sessions.fps')"
            :value="session.fps"
            :tip="t('sessions.tip_fps')"
          />
          <StatCell
            v-if="session.encoder_bitrate_kbps"
            :label="t('sessions.bitrate')"
            :value="formatBitrate(session.requested_bitrate_kbps || session.encoder_bitrate_kbps)"
            :sub-value="
              session.requested_bitrate_kbps &&
              session.requested_bitrate_kbps !== session.encoder_bitrate_kbps
                ? `${t('sessions.bitrate_encode_label')} ${formatBitrate(session.encoder_bitrate_kbps)}`
                : undefined
            "
            :tip="
              session.requested_bitrate_kbps &&
              session.requested_bitrate_kbps !== session.encoder_bitrate_kbps
                ? t('sessions.tip_bitrate_dual')
                : t('sessions.tip_bitrate')
            "
          />
          <StatCell
            v-if="session.codec"
            :label="t('sessions.codec')"
            :value="session.codec"
            :tip="t('sessions.tip_codec')"
          />
          <StatCell
            v-if="session.stream_gpu_model"
            :label="t('sessions.stream_gpu_model')"
            :value="session.stream_gpu_model"
            :tip="t('sessions.tip_stream_gpu_model')"
          />
          <StatCell
            v-if="session.audio_channels"
            :label="t('sessions.audio_channels')"
            :value="`${session.audio_channels}ch`"
            :tip="t('sessions.tip_audio_channels')"
          />
          <StatCell :label="t('sessions.encode_latency')" :tip="t('sessions.tip_encode_latency')">
            <span :class="encodeLatencyClass(session.encode_latency_ms)">
              {{ formatEncodeLatency(session.encode_latency_ms) }}
            </span>
          </StatCell>
          <StatCell
            :label="t('sessions.frames_sent')"
            :value="formatNumber(session.frames_sent)"
            :tip="t('sessions.tip_frames_sent')"
          />
          <StatCell
            :label="t('sessions.packets_sent')"
            :value="formatNumber(session.packets_sent)"
            :tip="t('sessions.tip_packets_sent')"
          />
          <StatCell
            :label="t('sessions.data_sent')"
            :value="formatBytes(session.bytes_sent)"
            :tip="t('sessions.tip_data_sent')"
          />
          <StatCell :label="t('sessions.client_losses')" :tip="t('sessions.tip_client_losses')">
            <span :class="session.client_reported_losses > 0 ? 'text-danger' : ''">
              {{ formatNumber(session.client_reported_losses) }}
            </span>
          </StatCell>
          <StatCell :label="t('sessions.idr_requests')" :tip="t('sessions.tip_idr_requests')">
            <span :class="session.idr_requests > 10 ? 'text-warning' : ''">
              {{ session.idr_requests }}
            </span>
          </StatCell>
          <StatCell
            :label="t('sessions.frame_invalidations')"
            :tip="t('sessions.tip_frame_invalidations')"
          >
            <span :class="session.invalidate_ref_count > 0 ? 'text-warning' : ''">
              {{ session.invalidate_ref_count }}
            </span>
          </StatCell>
          <StatCell
            :label="t('sessions.uptime')"
            :value="formatUptime(session.uptime_seconds)"
            :tip="t('sessions.tip_uptime')"
          />
        </div>
      </div>
    </div>

    <div class="flex items-center gap-2 mt-3">
      <n-button
        size="small"
        :type="showCharts ? 'primary' : 'default'"
        @click="$emit('update:showCharts', !showCharts)"
      >
        <i :class="['fas', showCharts ? 'fa-chart-line' : 'fa-chart-bar']" />
        <span class="ml-2">{{
          showCharts ? t('sessions.hide_charts') : t('sessions.show_charts')
        }}</span>
      </n-button>
    </div>

    <SessionCharts
      v-if="showCharts && sessions.length > 0"
      :session="sessions[0]"
      :session-id="sessions[0]?.uuid"
      protocol="rtsp"
    />
  </div>
  <div v-else-if="rtspCount > 0" class="mb-4">
    <div class="flex flex-wrap items-center gap-2 mb-3">
      <n-tag type="info" size="small" :bordered="false">RTSP</n-tag>
      <span class="text-sm font-medium">
        {{ t('sessions.rtsp_active', { count: rtspCount }) }}
      </span>
      <n-tag v-if="appRunning" type="success" size="small" :bordered="false" class="max-w-full">
        <span class="inline-flex min-w-0 max-w-full items-center">
          <i class="fas fa-gamepad mr-1 flex-shrink-0" />
          <span class="truncate">{{ appName || t('sessions.app_running') }}</span>
        </span>
      </n-tag>
    </div>
  </div>
</template>

<script setup lang="ts">
import { useI18n } from 'vue-i18n';
import { NButton, NTag } from 'naive-ui';
import type { RTSPSession } from '@/types/sessions';
import SessionCharts from '@/components/SessionCharts.vue';
import StatCell from '@/components/StatCell.vue';
import { formatBitrate, formatBytes, formatNumber, formatUptime } from './sessionFormatting';

defineProps<{
  sessions: RTSPSession[];
  rtspCount: number;
  appRunning: boolean;
  appName: string;
  showCharts: boolean;
}>();

defineEmits<{
  'update:showCharts': [value: boolean];
}>();

const { t } = useI18n();

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function encodeLatencyClass(value: unknown): string {
  if (!isFiniteNumber(value)) return '';
  if (value > 16) return 'text-danger';
  if (value > 8) return 'text-warning';
  return '';
}

function formatEncodeLatency(value: unknown): string {
  if (!isFiniteNumber(value)) return t('_common.unknown');
  return `${value.toFixed(1)}ms`;
}
</script>
