import { api } from './api/client';

const originalConsoleError = console.error;

export function initRemoteLogging() {
  console.error = (...args: any[]) => {
    originalConsoleError.apply(console, args);
    try {
      const message = args.map(arg => {
        if (arg instanceof Error) return arg.stack || arg.message;
        if (typeof arg === 'object') return JSON.stringify(arg);
        return String(arg);
      }).join(' ');
      
      // Fire and forget — we don't want to wait for the network on every error,
      // and we certainly don't want an infinite loop if the log call fails.
      api.sendClientLog('error', message).catch(() => {});
    } catch (e) {
      // ignore serialization errors
    }
  };

  window.addEventListener('unhandledrejection', (event) => {
    const reason = event.reason;
    const message = reason instanceof Error ? (reason.stack || reason.message) : String(reason);
    api.sendClientLog('error', `Unhandled Promise Rejection: ${message}`).catch(() => {});
  });

  window.addEventListener('error', (event) => {
    const message = event.error instanceof Error ? (event.error.stack || event.error.message) : event.message;
    api.sendClientLog('error', `Uncaught Error: ${message}`).catch(() => {});
  });
}
