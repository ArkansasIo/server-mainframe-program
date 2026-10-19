'use strict';

/* Browser API client for the MAINFRAME RailwayControl simulator. */
(function railwayApi(global) {
  async function request(path, options = {}) {
    const response = await fetch(path, {
      cache: 'no-store',
      ...options,
      headers: { Accept: 'application/json', ...(options.headers || {}) },
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  const api = {
    status: () => request('/api/railway/status'),
    trains: () => request('/api/railway/trains'),
    layout: () => request('/api/railway/layout'),
    events: (limit = 100) => request(`/api/railway/events?limit=${encodeURIComponent(limit)}`),
    alarms: () => request('/api/railway/alarms'),
    commands: () => request('/api/railway/commands'),
    command: (command, args = {}) => request('/api/railway/command', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ command, args }),
    }),
  };

  global.MFRailwayApi = api;
})(window);
