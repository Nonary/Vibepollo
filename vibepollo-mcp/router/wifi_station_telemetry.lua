-- Prometheus collector for per-station Wi-Fi telemetry from iw station dump.
-- Install as /usr/lib/lua/prometheus-collectors/wifi_station_telemetry.lua.
local ubus = require "ubus"

local function shell_quote(value)
  return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function number(value)
  return value and tonumber(value)
end

local function parse_station_dump(text, ifname)
  local stations = {}
  local station

  local function finish()
    if station and station.mac then
      stations[#stations + 1] = station
    end
  end

  for line in text:gmatch("[^\r\n]+") do
    local mac = line:match("^Station%s+(%S+)%s+")
    if mac then
      finish()
      station = {mac = mac, ifname = ifname}
    elseif station then
      local value
      value = line:match("^%s*tx retries:%s*(%d+)")
      if value then station.tx_retries = number(value) end
      value = line:match("^%s*tx failed:%s*(%d+)")
      if value then station.tx_failed = number(value) end
      value = line:match("^%s*rx drop misc:%s*(%d+)")
      if value then station.rx_drop_misc = number(value) end
      value = line:match("^%s*last ack signal:%s*(-?%d+)")
      if value then station.last_ack_signal = number(value) end
      value = line:match("^%s*avg ack signal:%s*(-?%d+)")
      if value then station.average_ack_signal = number(value) end

      local direction, rate, width = line:match(
        "^%s*(tx) bitrate:%s*([%d%.]+)%s+MBit/s%s+(%d+)%s*MHz")
      if not direction then
        direction, rate, width = line:match(
          "^%s*(rx) bitrate:%s*([%d%.]+)%s+MBit/s%s+(%d+)%s*MHz")
      end
      if direction then
        station[direction .. "_bitrate_kbps"] = number(rate) * 1000
        station[direction .. "_channel_width_mhz"] = {
          value = number(width),
          direction = direction,
        }

        local mcs = line:match("MCS%s+(%d+)")
        local nss = line:match("NSS%s+(%d+)")
        if mcs then
          station[direction .. "_mcs"] = {value = number(mcs), direction = direction}
        end
        if nss then
          station[direction .. "_nss"] = {value = number(nss), direction = direction}
        end
      end
    end
  end
  finish()
  return stations
end

local function scrape_interface(ifname, emit)
  local pipe = io.popen("iw dev " .. shell_quote(ifname) .. " station dump 2>/dev/null", "r")
  if not pipe then return end
  local text = pipe:read("*a") or ""
  pipe:close()
  for _, station in ipairs(parse_station_dump(text, ifname)) do
    local labels = {ifname = station.ifname, mac = station.mac}
    for name, value in pairs(station) do
      local metric_name = emit[name]
      if metric_name and value ~= nil then
        if type(value) == "table" then
          local direction_labels = {ifname = station.ifname, mac = station.mac, direction = value.direction}
          metric_name(direction_labels, value.value)
        else
          metric_name(labels, value)
        end
      end
    end
  end
end

local function scrape()
  local mcs = metric("wifi_station_mcs", "gauge")
  local nss = metric("wifi_station_nss", "gauge")
  local channel_width = metric("wifi_station_channel_width_mhz", "gauge")
  local metrics = {
    tx_retries = metric("wifi_station_tx_retries_total", "counter"),
    tx_failed = metric("wifi_station_tx_failed_total", "counter"),
    rx_drop_misc = metric("wifi_station_rx_drop_misc_total", "counter"),
    last_ack_signal = metric("wifi_station_last_ack_signal_dbm", "gauge"),
    average_ack_signal = metric("wifi_station_average_ack_signal_dbm", "gauge"),
    tx_bitrate_kbps = metric("wifi_station_tx_bitrate_kbps", "gauge"),
    rx_bitrate_kbps = metric("wifi_station_rx_bitrate_kbps", "gauge"),
    tx_channel_width_mhz = channel_width,
    rx_channel_width_mhz = channel_width,
    tx_mcs = mcs,
    rx_mcs = mcs,
    tx_nss = nss,
    rx_nss = nss,
  }

  local u = ubus.connect()
  if not u then return end
  local status = u:call("network.wireless", "status", {}) or {}
  for _, device in pairs(status) do
    for _, interface in ipairs(device.interfaces or {}) do
      if interface.ifname then scrape_interface(interface.ifname, metrics) end
    end
  end
  u:close()
end

return {scrape = scrape}
