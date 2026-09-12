import { makeAutoObservable, runInAction } from 'mobx';
import { api } from '../api/client';

// Mirrors hephaestus/src/stream/GpuMetrics.h — encoder_util_pct/
// decoder_util_pct only ever come from the "nvidia" backend (nvidia-smi
// exposes separate NVENC/NVDEC engine utilization; there's no AMD
// equivalent already wired up), name/temp_c are best-effort on both. Absent
// entirely (ComponentMetrics.gpu undefined) when hw-accel isn't resolved to
// a real GPU backend at all — see ActivityPage.tsx's own comment on why
// that's "draw nothing," not a zeroed-out card.
export interface GpuMetrics {
  backend: 'nvidia' | 'amd';
  name?: string;
  gpu_util_pct: number;
  encoder_util_pct?: number;
  decoder_util_pct?: number;
  mem_used_mb: number;
  mem_total_mb: number;
  temp_c?: number;
}

export interface ComponentMetrics {
  cpu_usage: number;
  ram_bytes: number;
  gpu?: GpuMetrics;
}

export interface SystemMetrics {
  cpu_usage: number;
  ram_total: number;
  ram_free: number;
}

export interface AggregatedMetrics {
  hermes: ComponentMetrics;
  kairos: ComponentMetrics;
  hephaestus: ComponentMetrics;
  system: SystemMetrics;
}

export class MetricsStore {
  history: AggregatedMetrics[] = [];
  maxHistory = 60; // 1 minute at 1s intervals

  constructor() {
    makeAutoObservable(this);
  }

  async poll() {
    try {
      const data = await api.getSystemMetrics();
      runInAction(() => {
        this.history.push(normalize(data));
        if (this.history.length > this.maxHistory) {
          this.history.shift();
        }
      });
    } catch (e) {
      console.error('Failed to poll metrics', e);
    }
  }
}

// Hermes aggregates kairos/hephaestus metrics over internal HTTP calls with a
// 1s timeout (Router.cpp); a slow or busy peer (e.g. kairos mid-sync) makes
// that leg come back as `{}`. Coerce every field to a finite number here so
// a partial response can never put NaN into a chart's polyline points.
function num(v: unknown): number {
  return typeof v === 'number' && Number.isFinite(v) ? v : 0;
}

function normalizeGpu(g: any): GpuMetrics | undefined {
  if (!g || (g.backend !== 'nvidia' && g.backend !== 'amd')) return undefined;
  return {
    backend:           g.backend,
    name:              typeof g.name === 'string' ? g.name : undefined,
    gpu_util_pct:      num(g.gpu_util_pct),
    encoder_util_pct:  typeof g.encoder_util_pct === 'number' ? g.encoder_util_pct : undefined,
    decoder_util_pct:  typeof g.decoder_util_pct === 'number' ? g.decoder_util_pct : undefined,
    mem_used_mb:       num(g.mem_used_mb),
    mem_total_mb:      num(g.mem_total_mb),
    temp_c:            typeof g.temp_c === 'number' ? g.temp_c : undefined,
  };
}

function normalizeComponent(c: any): ComponentMetrics {
  return { cpu_usage: num(c?.cpu_usage), ram_bytes: num(c?.ram_bytes), gpu: normalizeGpu(c?.gpu) };
}

function normalize(data: any): AggregatedMetrics {
  return {
    hermes:     normalizeComponent(data?.hermes),
    kairos:     normalizeComponent(data?.kairos),
    hephaestus: normalizeComponent(data?.hephaestus),
    system: {
      cpu_usage: num(data?.system?.cpu_usage),
      ram_total: num(data?.system?.ram_total),
      ram_free:  num(data?.system?.ram_free),
    },
  };
}

export const metricsStore = new MetricsStore();
