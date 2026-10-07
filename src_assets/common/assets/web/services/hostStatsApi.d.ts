import type { HostInfo, HostStatsSnapshot } from '../types/host';

export declare function fetchHostStats(): Promise<HostStatsSnapshot>;
export declare function fetchHostInfo(): Promise<HostInfo>;
