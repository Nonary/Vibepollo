<template>
  <div class="grid min-w-0 gap-5 md:grid-cols-2">
    <section class="min-w-0 space-y-3">
      <h3 class="text-lg font-semibold">{{ $t('resource_card.resources') || 'Resources' }}</h3>
      <div class="space-y-2.5">
        <n-card
          v-for="item in resources"
          :key="item.href"
          hoverable
          tag="a"
          size="small"
          class="resource-link-card"
          :href="item.href"
          target="_blank"
          rel="noopener noreferrer"
        >
          <div class="flex min-w-0 items-center gap-3">
            <span
              class="inline-flex h-10 w-10 shrink-0 items-center justify-center rounded-full"
              :style="item.avatarStyle"
            >
              <i :class="[item.icon, 'text-[18px]']" />
            </span>
            <div class="min-w-0 flex-1 space-y-0.5">
              <n-text strong class="block break-words">{{ item.title }}</n-text>
              <n-text depth="3" class="block break-words">{{ item.description }}</n-text>
            </div>
          </div>
        </n-card>
      </div>
    </section>

    <section class="min-w-0 space-y-3">
      <h3 class="text-lg font-semibold">{{ $t('resource_card.legal') }}</h3>
      <n-text depth="3" class="block leading-relaxed">{{ $t('resource_card.legal_desc') }}</n-text>
      <div class="space-y-2.5">
        <n-card
          v-for="item in legalLinks"
          :key="item.href"
          hoverable
          tag="a"
          size="small"
          class="resource-link-card"
          :href="item.href"
          target="_blank"
          rel="noopener noreferrer"
        >
          <div class="flex min-w-0 items-center gap-3">
            <span
              class="inline-flex h-10 w-10 shrink-0 items-center justify-center rounded-full"
              :style="item.avatarStyle"
            >
              <i :class="[item.icon, 'text-[18px]']" />
            </span>
            <div class="min-w-0 flex-1 space-y-0.5">
              <n-text strong class="block break-words">{{ item.title }}</n-text>
              <n-text depth="3" class="block break-words">{{ item.description }}</n-text>
            </div>
          </div>
        </n-card>
      </div>
    </section>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { NCard, NText } from 'naive-ui';

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
    href: 'https://github.com/Nonary/Vibepollo/discussions',
    icon: 'fab fa-github',
    title: t('resource_card.github_discussions'),
    description: 'GitHub Discussions',
    avatarStyle: 'background-color: rgba(16, 185, 129, 0.15); color: rgb(16, 185, 129);',
  },
  {
    href: 'https://github.com/Nonary/Vibepollo/issues',
    icon: 'fab fa-github',
    title: 'GitHub Issues',
    description: 'Report bugs or request features',
    avatarStyle: 'background-color: rgba(59, 130, 246, 0.15); color: rgb(59, 130, 246);',
  },
]);

const legalLinks = computed(() => [
  {
    href: 'https://github.com/Nonary/Vibepollo/blob/master/LICENSE',
    icon: 'fas fa-file-alt',
    title: t('resource_card.license'),
    description: 'View license',
    avatarStyle: 'background-color: rgba(34, 197, 94, 0.15); color: rgb(34, 197, 94);',
  },
  {
    href: 'https://github.com/Nonary/Vibepollo/blob/master/NOTICE',
    icon: 'fas fa-exclamation',
    title: t('resource_card.third_party_notice'),
    description: 'Third-party notices',
    avatarStyle: 'background-color: rgba(248, 113, 113, 0.15); color: rgb(248, 113, 113);',
  },
]);
</script>

<style scoped>
.resource-link-card {
  display: block;
  width: 100%;
  min-width: 0;
}
</style>
