/**
 * Host system stats types — mirrors `platf::host_stats_t` /
 * `platf::host_info_t` from the C++ side.
 */
export interface HostStatsSnapshot {
  cpu_percent: number;
  cpu_temp_c: number;
  ram_used_bytes: number;
  ram_total_bytes: number;
  ram_percent: number;
  gpu_percent: number;
  gpu_encoder_percent: number;
  gpu_temp_c: number;
  vram_used_bytes: number;
  vram_total_bytes: number;
  vram_percent: number;
}

export interface HostInfo {
  cpu_model: string;
  gpu_model: string;
  cpu_logical_cores: number;
  ram_total_bytes: number;
  vram_total_bytes: number;
}

export interface HostHistoryPoint {
  timestamp: number;
  cpu_percent: number;
  gpu_percent: number;
  gpu_encoder_percent: number;
  ram_percent: number;
  vram_percent: number;
}
