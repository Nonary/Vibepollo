const gridColor = 'rgba(128, 128, 128, 0.15)';
const tickColor = 'rgba(128, 128, 128, 0.6)';

export function buildBaseChartOptions(eventAnnotations: Record<string, unknown>) {
  const hasAnnotations = Object.keys(eventAnnotations).length > 0;
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 0 },
    interaction: { mode: 'index' as const, intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgba(0, 0, 0, 0.8)',
        titleColor: '#fff',
        bodyColor: '#fff',
        padding: 8,
        cornerRadius: 6,
      },
      ...(hasAnnotations ? { annotation: { annotations: eventAnnotations } } : {}),
    },
    scales: {
      x: {
        grid: { color: gridColor },
        ticks: { color: tickColor, maxTicksLimit: 8, maxRotation: 0, font: { size: 10 } },
      },
      y: {
        beginAtZero: true,
        grid: { color: gridColor },
        ticks: { color: tickColor, font: { size: 10 } },
      },
    },
    elements: {
      point: { radius: 0, hitRadius: 8 },
      line: { tension: 0.3, borderWidth: 2 },
    },
  };
}

export function buildLatencyChartOptions(base: ReturnType<typeof buildBaseChartOptions>) {
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: { display: false },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: 20,
        ticks: { ...base.scales.y.ticks, callback: (value: number) => `${value}ms` },
      },
    },
  };
}

export function buildQualityChartOptions(base: ReturnType<typeof buildBaseChartOptions>) {
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        ticks: {
          ...base.scales.y.ticks,
          stepSize: 1,
          callback: (value: number) => (Math.floor(value) === value ? value : ''),
        },
      },
    },
  };
}

export function buildFpsChartOptions(
  base: ReturnType<typeof buildBaseChartOptions>,
  targetFps: number,
) {
  return {
    ...base,
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: targetFps + 5,
        ticks: { ...base.scales.y.ticks, callback: (value: number) => `${value}` },
      },
    },
  };
}

export function buildHostPercentChartOptions(base: ReturnType<typeof buildBaseChartOptions>) {
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: 100,
        ticks: { ...base.scales.y.ticks, callback: (value: number) => `${value}%` },
      },
    },
  };
}

export function buildHostNetworkChartOptions(base: ReturnType<typeof buildBaseChartOptions>) {
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        beginAtZero: true,
        ticks: { ...base.scales.y.ticks, callback: (value: number) => `${value} Mbps` },
      },
    },
  };
}

const zoomPluginConfig = {
  pan: { enabled: true, mode: 'x' as const, modifierKey: 'shift' as const },
  zoom: {
    wheel: { enabled: true },
    pinch: { enabled: true },
    drag: { enabled: false },
    mode: 'x' as const,
  },
  limits: {
    x: { minRange: 2 },
  },
};

export function withZoom<T extends { plugins?: Record<string, unknown> }>(options: T): T {
  return {
    ...options,
    plugins: {
      ...(options.plugins ?? {}),
      zoom: zoomPluginConfig,
    },
  };
}
