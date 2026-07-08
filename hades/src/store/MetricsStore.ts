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
        this.history.push(data);
        if (this.history.length > this.maxHistory) {
          this.history.shift();
        }
      });
    } catch (e) {
      console.error('Failed to poll metrics', e);
    }
  }
}

export const metricsStore = new MetricsStore();
