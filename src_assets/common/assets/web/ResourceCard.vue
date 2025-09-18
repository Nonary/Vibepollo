<template>
  <n-space vertical size="large">
    <n-grid cols="24" x-gap="16" y-gap="20" responsive="screen">
      <n-gi :span="12" :s="24">
        <n-space vertical size="medium">
          <n-h3>{{ $t('resource_card.resources') || 'Resources' }}</n-h3>
          <n-space vertical size="small">
            <n-card
              v-for="item in resources"
              :key="item.href"
              hoverable
              tag="a"
              size="small"
              :href="item.href"
              target="_blank"
              rel="noopener noreferrer"
            >
              <n-space align="center" size="large">
                <span
                  class="shrink-0 inline-flex h-10 w-10 items-center justify-center rounded-full"
                  :style="item.avatarStyle"
                >
                  <i :class="[item.icon, 'text-[18px]']" />
                </span>
                <n-space vertical size="small">
                  <n-text strong>{{ item.title }}</n-text>
                  <n-text depth="3">{{ item.description }}</n-text>
                </n-space>
              </n-space>
            </n-card>
          </n-space>
        </n-space>
      </n-gi>

      <n-gi :span="12" :s="24">
        <n-space vertical size="medium">
          <n-h3>{{ $t('resource_card.legal') }}</n-h3>
          <n-text depth="3">{{ $t('resource_card.legal_desc') }}</n-text>
          <n-space vertical size="small">
            <n-card
              v-for="item in legalLinks"
              :key="item.href"
              hoverable
              tag="a"
              size="small"
              :href="item.href"
              target="_blank"
              rel="noopener noreferrer"
            >
              <n-space align="center" size="large">
                <span
                  class="shrink-0 inline-flex h-10 w-10 items-center justify-center rounded-full"
                  :style="item.avatarStyle"
                >
                  <i :class="[item.icon, 'text-[18px]']" />
                </span>
                <n-space vertical size="small">
                  <n-text strong>{{ item.title }}</n-text>
                  <n-text depth="3">{{ item.description }}</n-text>
                </n-space>
              </n-space>
            </n-card>
          </n-space>
        </n-space>
      </n-gi>
    </n-grid>
  </n-space>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NGrid, NGi, NCard, NSpace, NH3, NText } from 'naive-ui';

const { t } = useI18n();

function withFallback(key: string, fallback: string) {
  const value = t(key);
  return value === key ? fallback : value;
}

const resources = computed(() => [
  {
    href: 'https://moonlight-stream.org/discord',
    icon: 'fab fa-discord',
    title: 'Discord',
    description: withFallback('resource_card.discord_desc', 'Join the community'),
    avatarStyle: 'background-color: rgba(99, 102, 241, 0.15); color: rgb(99, 102, 241);',
  },
  {
    href: 'https://github.com/Nonary/vibeshine/discussions',
    icon: 'fab fa-github',
    title: t('resource_card.github_discussions'),
    description: 'GitHub Discussions',
    avatarStyle: 'background-color: rgba(16, 185, 129, 0.15); color: rgb(16, 185, 129);',
  },
  {
    href: 'https://github.com/Nonary/vibeshine/issues',
    icon: 'fab fa-github',
    title: 'GitHub Issues',
    description: 'Report bugs or request features',
    avatarStyle: 'background-color: rgba(59, 130, 246, 0.15); color: rgb(59, 130, 246);',
  },
]);

const legalLinks = computed(() => [
  {
    href: 'https://github.com/Nonary/vibeshine/blob/master/LICENSE',
    icon: 'fas fa-file-alt',
    title: t('resource_card.license'),
    description: 'View license',
    avatarStyle: 'background-color: rgba(34, 197, 94, 0.15); color: rgb(34, 197, 94);',
  },
  {
    href: 'https://github.com/Nonary/vibeshine/blob/master/NOTICE',
    icon: 'fas fa-exclamation',
    title: t('resource_card.third_party_notice'),
    description: 'Third-party notices',
    avatarStyle: 'background-color: rgba(248, 113, 113, 0.15); color: rgb(248, 113, 113);',
  },
]);
</script>
