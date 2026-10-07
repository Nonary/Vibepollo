<template>
  <div v-if="sessions.length > 0">
    <div class="flex flex-wrap items-center gap-2 mb-3">
      <n-tag type="warning" size="small" :bordered="false">WebRTC</n-tag>
      <span class="text-sm font-medium">
        {{ t('sessions.webrtc_active', { count: sessions.length }) }}
      </span>
    </div>

    <div class="space-y-3">
      <div
        v-for="session in sessions"
        :key="session.id"
        class="rounded-xl border border-dark/[0.06] bg-light/[0.03] p-4 dark:border-light/[0.10] dark:bg-dark/[0.06]"
      >
        <div class="flex flex-wrap items-center gap-2 mb-3">
          <span class="text-sm font-semibold">{{ session.id.substring(0, 8) }}</span>
          <n-tag v-if="session.video" type="success" size="small" :bordered="false">
            <span class="inline-flex items-center"
              ><i class="fas fa-video mr-1" />{{ t('sessions.video') }}</span
            >
          </n-tag>
          <n-tag v-if="session.audio" type="success" size="small" :bordered="false">
            <span class="inline-flex items-center"
              ><i class="fas fa-volume-up mr-1" />{{ t('sessions.audio') }}</span
            >
          </n-tag>
          <n-tag v-if="session.encoded" type="info" size="small" :bordered="false">
            {{ t('sessions.encoded') }}
          </n-tag>
          <n-tag v-if="session.hdr" type="warning" size="small" :bordered="false">HDR</n-tag>
          <n-tag v-if="session.yuv444" type="info" size="small" :bordered="false">YUV444</n-tag>
        </div>

        <div class="grid grid-cols-1 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-6 gap-3">
          <StatCell
            v-if="session.width && session.height"
            :label="t('sessions.resolution')"
            :value="`${session.width}×${session.height}`"
            :tip="t('sessions.tip_resolution')"
          />
          <StatCell
            v-if="session.fps != null"
            :label="t('sessions.fps')"
            :value="session.fps"
            :tip="t('sessions.tip_fps')"
          />
          <StatCell
            v-if="session.encoder_bitrate_kbps != null"
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
            :label="t('sessions.video_packets')"
            :value="formatNumber(session.video_packets)"
            :tip="t('sessions.tip_video_packets')"
          />
          <StatCell
            :label="t('sessions.audio_packets')"
            :value="formatNumber(session.audio_packets)"
            :tip="t('sessions.tip_audio_packets')"
          />
          <StatCell :label="t('sessions.video_dropped')" :tip="t('sessions.tip_video_dropped')">
            <span :class="session.video_dropped > 0 ? 'text-danger' : ''">
              {{ formatNumber(session.video_dropped) }}
            </span>
          </StatCell>
          <StatCell :label="t('sessions.audio_dropped')" :tip="t('sessions.tip_audio_dropped')">
            <span :class="session.audio_dropped > 0 ? 'text-danger' : ''">
              {{ formatNumber(session.audio_dropped) }}
            </span>
          </StatCell>
          <StatCell
            :label="t('sessions.video_queue')"
            :value="session.video_queue_frames"
            :tip="t('sessions.tip_video_queue')"
          />
          <StatCell
            :label="t('sessions.audio_queue')"
            :value="session.audio_queue_frames"
            :tip="t('sessions.tip_audio_queue')"
          />
          <StatCell
            :label="t('sessions.inflight')"
            :value="session.video_inflight_frames"
            :tip="t('sessions.tip_inflight')"
          />
          <StatCell
            v-if="session.audio_codec"
            :label="t('sessions.audio_codec')"
            :value="session.audio_codec"
            :tip="t('sessions.tip_audio_codec')"
          />
          <StatCell
            v-if="session.profile"
            :label="t('sessions.profile')"
            :value="session.profile"
            :tip="t('sessions.tip_profile')"
          />
          <StatCell
            v-if="session.last_video_age_ms != null"
            :label="t('sessions.last_video')"
            :value="`${session.last_video_age_ms}ms`"
            :tip="t('sessions.tip_last_video')"
          />
          <StatCell
            v-if="session.last_audio_age_ms != null"
            :label="t('sessions.last_audio')"
            :value="`${session.last_audio_age_ms}ms`"
            :tip="t('sessions.tip_last_audio')"
          />
        </div>
      </div>
    </div>

    <div class="flex justify-end mt-3">
      <n-button size="tiny" quaternary @click="$emit('update:showCharts', !showCharts)">
        <i :class="['fas', showCharts ? 'fa-chart-line' : 'fa-chart-bar']" />
        <span class="ml-1 text-xs">{{
          showCharts ? t('sessions.hide_charts') : t('sessions.show_charts')
        }}</span>
      </n-button>
    </div>

    <SessionCharts
      v-if="showCharts && sessions.length > 0"
      :session="sessions[0]"
      :session-id="sessions[0]?.id"
      protocol="webrtc"
    />
  </div>
</template>

<script setup lang="ts">
import { useI18n } from 'vue-i18n';
import { NButton, NTag } from 'naive-ui';
import type { WebRTCSession } from '@/types/sessions';
import SessionCharts from '@/components/SessionCharts.vue';
import StatCell from '@/components/StatCell.vue';
import { formatBitrate, formatNumber } from './sessionFormatting';

defineProps<{
  sessions: WebRTCSession[];
  showCharts: boolean;
}>();

defineEmits<{
  'update:showCharts': [value: boolean];
}>();

const { t } = useI18n();
</script>
