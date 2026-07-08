import { makeAutoObservable, runInAction } from 'mobx';
import { api } from '../api/client';

export interface ComponentMetrics {
  cpu_usage: number;
  ram_bytes: number;
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

function normalizeComponent(c: any): ComponentMetrics {
  return { cpu_usage: num(c?.cpu_usage), ram_bytes: num(c?.ram_bytes) };
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
