# OpenWrt Wi-Fi station collector

`wifi_station_telemetry.lua` is an additional
`prometheus-node-exporter-lua` collector. It reads `iw station dump` and
exports per-station retry, ACK-signal, bitrate, MCS, NSS, and channel-width
metrics.

## Install on the router

Run from the repository root:

```sh
scp router/wifi_station_telemetry.lua root@192.168.1.1:/usr/lib/lua/prometheus-collectors/
curl -sS http://192.168.1.1:9100/metrics \
  | grep -E '^wifi_station_(tx_retries|tx_failed|rx_drop_misc|last_ack_signal|average_ack_signal|tx_bitrate|rx_bitrate|mcs|nss|channel_width)'
```

The exporter loads every `*.lua` file in the collector directory at process
startup, so restart the exporter after copying the file, then scrape again.

All station metrics have `ifname` and `mac` labels. `wifi_station_mcs`,
`wifi_station_nss`, and `wifi_station_channel_width_mhz` additionally have
`direction="tx"` or `direction="rx"`. Bitrates are exported in kilobits per
second; `iw` reports them in Mbit/s.
