'use strict';

/** Safe web simulator for the MAINFRAME RailwayControl UI. Not a real railway controller. */
const MAX_EVENTS = 250;
const now = () => new Date().toISOString();

function makeState() {
  return {
    system: { name: 'MAINFRAME RAILWAY CONTROL', mode: 'SIMULATION', interlocking: 'ACTIVE', atc: 'ACTIVE', trackCircuits: 'ACTIVE', emergency: false },
    sections: [
      { id: 'T1', name: 'North Platform', occupied: true, locked: true },
      { id: 'T2', name: 'North Junction', occupied: false, locked: true },
      { id: 'T3', name: 'Freight Siding', occupied: true, locked: false },
      { id: 'T4', name: 'Central Main', occupied: false, locked: true },
      { id: 'T5', name: 'Depot Lead', occupied: true, locked: false },
      { id: 'T6', name: 'South Platform', occupied: false, locked: false },
    ],
    points: [
      { id: 'P1', name: 'North Junction', position: 'NORMAL', locked: true },
      { id: 'P2', name: 'Depot Turnout', position: 'REVERSE', locked: false },
    ],
    signals: [
      { id: 'S1', name: 'North Home', aspect: 'GREEN', section: 'T1' },
      { id: 'S2', name: 'Junction', aspect: 'YELLOW', section: 'T2' },
      { id: 'S3', name: 'Freight', aspect: 'RED', section: 'T3' },
      { id: 'S4', name: 'Central', aspect: 'GREEN', section: 'T4' },
      { id: 'S5', name: 'Depot', aspect: 'RED', section: 'T5' },
      { id: 'S6', name: 'South Home', aspect: 'RED', section: 'T6' },
    ],
    trains: [
      { id: '1A34', className: 'PASSENGER', state: 'READY', section: 'T1', speedKph: 0, authority: 'R2', destination: 'T6', consist: 6 },
      { id: '2B17', className: 'FREIGHT', state: 'HELD', section: 'T3', speedKph: 0, authority: 'NONE', destination: 'T5', consist: 42 },
      { id: '3C05', className: 'MAINTENANCE', state: 'READY', section: 'T5', speedKph: 0, authority: 'NONE', destination: 'T2', consist: 3 },
    ],
    routes: [
      { id: 'R1', from: 'S1', to: 'S2', state: 'RELEASED', locked: false, train: null },
      { id: 'R2', from: 'S2', to: 'S4', state: 'SET', locked: true, train: '1A34' },
      { id: 'R3', from: 'S5', to: 'S6', state: 'RELEASED', locked: false, train: null },
    ],
    alarms: [], events: [], startedAt: now(),
  };
}

class RailwaySimulator {
  constructor() { this.state = makeState(); this.record('SYSTEM', 'RailwayControl web simulator initialized'); }
  record(type, message, severity = 'INFO') {
    this.state.events.unshift({ timestamp: now(), type, severity, message });
    this.state.events = this.state.events.slice(0, MAX_EVENTS);
  }
  snapshot() { return JSON.parse(JSON.stringify(this.state)); }
  reject(message, detail = '') {
    this.record('CONTROL', `${message}${detail ? `: ${detail}` : ''}`, 'WARN');
    return { ok: false, error: message, detail };
  }
  command(command, args = {}) {
    const op = String(command || '').trim().toUpperCase();
    const train = args.train ? this.state.trains.find((x) => x.id === String(args.train).toUpperCase()) : null;
    if (op === 'LIST_TRAINS') return { ok: true, result: this.state.trains };
    if (op === 'STATUS') return { ok: true, result: this.snapshot() };
    if (op === 'DISPATCH') {
      const route = this.state.routes.find((x) => x.id === String(args.route || 'R2').toUpperCase());
      if (!route) return this.reject('Unknown route', args.route);
      if (route.locked && route.train && route.train !== (train && train.id)) return this.reject('Route is already locked', route.id);
      route.state = 'SET'; route.locked = true; route.train = train ? train.id : '1A34';
      this.record('ROUTE', `Route ${route.id} dispatched for ${route.train}`);
      return { ok: true, result: route };
    }
    if (op === 'RUN') {
      if (!train) return this.reject('Unknown train', args.train);
      if (this.state.system.emergency) return this.reject('Emergency stop is active');
      train.state = 'RUNNING'; train.speedKph = Math.min(40, Math.max(5, Number(args.speed) || 25));
      this.record('TRAIN', `${train.id} running at ${train.speedKph} km/h`);
      return { ok: true, result: train };
    }
    if (op === 'HOLD') {
      if (!train) return this.reject('Unknown train', args.train);
      train.state = 'HELD'; train.speedKph = 0; this.record('TRAIN', `${train.id} held`);
      return { ok: true, result: train };
    }
    if (op === 'ESTOP') {
      this.state.system.emergency = true;
      this.state.trains.forEach((x) => { x.state = 'EMERGENCY_STOP'; x.speedKph = 0; });
      this.state.alarms.unshift({ timestamp: now(), code: 'E_STOP', message: 'Simulation emergency stop active' });
      this.record('SAFETY', 'Emergency stop activated', 'ALARM');
      return { ok: true, result: { emergency: true } };
    }
    if (op === 'RESET_ESTOP') {
      this.state.system.emergency = false;
      this.state.trains.forEach((x) => { if (x.state === 'EMERGENCY_STOP') x.state = 'HELD'; });
      this.record('SAFETY', 'Emergency stop reset; trains remain held');
      return { ok: true, result: { emergency: false } };
    }
    if (op === 'CLEAR_ALARMS') { this.state.alarms = []; this.record('SAFETY', 'Alarm list cleared'); return { ok: true, result: [] }; }
    return this.reject('Unsupported simulator command', op);
  }
}

module.exports = { RailwaySimulator };
