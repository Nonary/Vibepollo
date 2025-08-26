<template>
  <div class="space-y-6">
    <n-grid cols="24" x-gap="16" y-gap="20" responsive="screen">
      <!-- Left: Resources/Web links -->
      <n-gi :span="12" :s="24">
        <div class="space-y-3">
          <h3 class="text-sm font-semibold">{{ $t('resource_card.resources') || 'Resources' }}</h3>
          <div class="space-y-2">
            <a
              href="https://moonlight-stream.org/discord"
              target="_blank"
              class="group p-3 md:p-4 rounded-lg border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-sm transition flex items-center gap-3 md:gap-4"
            >
              <span
                class="shrink-0 inline-flex items-center justify-center h-10 w-10 rounded-full bg-accent/15 text-accent"
              >
                <i class="fab fa-discord text-[18px]" />
              </span>
              <div class="space-y-0.5 text-left">
                <div class="text-sm font-semibold leading-snug">Discord</div>
                <div class="text-xs opacity-70 leading-snug">Join the community</div>
              </div>
            </a>
            <a
              href="https://github.com/Nonary/vibeshine/discussions"
              target="_blank"
              class="group p-3 md:p-4 rounded-lg border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-sm transition flex items-center gap-3 md:gap-4"
            >
              <span
                class="shrink-0 inline-flex items-center justify-center h-10 w-10 rounded-full bg-brand/15 text-brand"
              >
                <i class="fab fa-github text-[18px]" />
              </span>
              <div class="space-y-0.5 text-left">
                <div class="text-sm font-semibold leading-snug">
                  {{ $t('resource_card.github_discussions') }}
                </div>
                <div class="text-xs opacity-70 leading-snug">GitHub Discussions</div>
              </div>
            </a>
            <a
              href="https://github.com/Nonary/vibeshine/issues"
              target="_blank"
              class="group p-3 md:p-4 rounded-lg border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-sm transition flex items-center gap-3 md:gap-4"
            >
              <span
                class="shrink-0 inline-flex items-center justify-center h-10 w-10 rounded-full bg-brand/15 text-brand"
              >
                <i class="fab fa-github text-[18px]" />
              </span>
              <div class="space-y-0.5 text-left">
                <div class="text-sm font-semibold leading-snug">GitHub Issues</div>
                <div class="text-xs opacity-70 leading-snug">Report bugs or request features</div>
              </div>
            </a>
          </div>
        </div>
      </n-gi>

      <!-- Right: Legal -->
      <n-gi :span="12" :s="24">
        <div class="space-y-3">
          <h3 class="text-sm font-semibold">{{ $t('resource_card.legal') }}</h3>
          <p class="text-xs opacity-70 leading-relaxed">
            {{ $t('resource_card.legal_desc') }}
          </p>
          <div class="space-y-2">
            <a
              href="https://github.com/Nonary/vibeshine/blob/master/LICENSE"
              target="_blank"
              class="group p-3 md:p-4 rounded-lg border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-sm transition flex items-center gap-3 md:gap-4"
            >
              <span
                class="shrink-0 inline-flex items-center justify-center h-10 w-10 rounded-full bg-success/15 text-success"
              >
                <i class="fas fa-file-alt text-[18px]" />
              </span>
              <div class="space-y-0.5 text-left">
                <div class="text-sm font-semibold leading-snug">
                  {{ $t('resource_card.license') }}
                </div>
                <div class="text-xs opacity-70 leading-snug">View license</div>
              </div>
            </a>
            <a
              href="https://github.com/Nonary/vibeshine/blob/master/NOTICE"
              target="_blank"
              class="group p-3 md:p-4 rounded-lg border border-dark/10 dark:border-light/10 bg-light/70 dark:bg-surface/70 hover:bg-light/90 dark:hover:bg-surface/80 hover:shadow-sm transition flex items-center gap-3 md:gap-4"
            >
              <span
                class="shrink-0 inline-flex items-center justify-center h-10 w-10 rounded-full bg-danger/15 text-danger"
              >
                <i class="fas fa-exclamation text-[18px]" />
              </span>
              <div class="space-y-0.5 text-left">
                <div class="text-sm font-semibold leading-snug">
                  {{ $t('resource_card.third_party_notice') }}
                </div>
                <div class="text-xs opacity-70 leading-snug">Third-party notices</div>
              </div>
            </a>
          </div>
        </div>
      </n-gi>
    </n-grid>
  </div>
</template>

<script setup lang="ts">
import { NGrid, NGi } from 'naive-ui';
</script>
