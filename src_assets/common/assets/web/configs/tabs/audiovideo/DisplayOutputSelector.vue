<script setup>
import { ref, computed, onMounted } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const outputNamePlaceholder = (props.platform === 'windows') ? '{de9bb7e2-186e-505b-9e93-f48793333810}' : '0'

const devices = ref([])
const loading = ref(false)
const loadError = ref('')

const normalizedDevices = computed(() => {
  // Normalize to an array of { id, name, display }
  return devices.value.map(d => {
    const id = d.device_id ?? d.id ?? ''
    const display = d.display_name ?? d.display ?? ''
    const name = d.friendly_name ?? d.name ?? display ?? id
    return { id, name, display }
  }).filter(d => d.id)
})

const selectedDevice = computed(() => {
  if (!config.value?.output_name) return null
  return normalizedDevices.value.find(d => d.id === config.value.output_name) || null
})

onMounted(async () => {
  loading.value = true
  loadError.value = ''
  try {
    const r = await fetch('./api/display-devices')
    const data = await r.json()
    // Handle either an array or an object wrapper
    if (Array.isArray(data)) {
      devices.value = data
    } else if (data && Array.isArray(data.devices)) {
      devices.value = data.devices
    } else if (data && data.status === false) {
      loadError.value = data.error || 'Failed to load display devices.'
    } else {
      // Unexpected shape; fallback gracefully
      devices.value = []
    }
  } catch (e) {
    loadError.value = String(e)
  } finally {
    loading.value = false
  }
})
</script>

<template>
  <div class="mb-3">
    <label for="output_name" class="form-label">{{ $tp('config.output_name') }}</label>

    <!-- Prefer dropdown when devices are available; fallback to manual input -->
    <template v-if="!loading && !loadError && normalizedDevices.length > 0">
      <select id="output_name" class="form-select" v-model="config.output_name">
        <option :value="''">{{ platform === 'windows' ? 'Primary display (auto)' : 'Default (auto)' }}</option>
        <option v-for="d in normalizedDevices" :key="d.id" :value="d.id">
          {{ d.name }}
          <template v-if="d.display"> — {{ d.display }}</template>
        </option>
      </select>
      <div class="form-text" v-if="selectedDevice">
        <div><b>Monitor:</b> {{ selectedDevice.name }} <span v-if="selectedDevice.display">({{ selectedDevice.display }})</span></div>
        <div class="monospace"><b>ID:</b> {{ selectedDevice.id }}</div>
      </div>
      <div class="form-text" v-else>
        {{ $tp('config.output_name_desc') }}
      </div>
    </template>

    <template v-else>
      <input type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
             v-model="config.output_name"/>
      <div class="form-text">
        <template v-if="loading">Detecting displays…</template>
        <template v-else-if="loadError">{{ loadError }}</template>
        <template v-else>
          {{ $tp('config.output_name_desc') }}<br>
          <PlatformLayout :platform="platform">
            <template #windows>
              <pre style="white-space: pre-line;">
                <b>&nbsp;&nbsp;{</b>
                <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
                <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
                <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
                <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
                <b>&nbsp;&nbsp;}</b>
              </pre>
            </template>
            <template #linux>
              <pre style="white-space: pre-line;">
                Info: Detecting displays
                Info: Detected display: DVI-D-0 (id: 0) connected: false
                Info: Detected display: HDMI-0 (id: 1) connected: true
                Info: Detected display: DP-0 (id: 2) connected: true
                Info: Detected display: DP-1 (id: 3) connected: false
                Info: Detected display: DVI-D-1 (id: 4) connected: false
              </pre>
            </template>
            <template #macos>
              <pre style="white-space: pre-line;">
                Info: Detecting displays
                Info: Detected display: Monitor-0 (id: 3) connected: true
                Info: Detected display: Monitor-1 (id: 2) connected: true
              </pre>
            </template>
          </PlatformLayout>
        </template>
      </div>
    </template>
  </div>
</template>
