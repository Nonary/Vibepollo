import { http } from '@/http';
import { HostInfo, HostStatsSnapshot } from '@/types/host';

export async function fetchHostStats(): Promise<HostStatsSnapshot> {
  const { data } = await http.get<HostStatsSnapshot>('/api/host/stats');
  return data;
}

export async function fetchHostInfo(): Promise<HostInfo> {
  const { data } = await http.get<HostInfo>('/api/host/info');
  return data;
}
