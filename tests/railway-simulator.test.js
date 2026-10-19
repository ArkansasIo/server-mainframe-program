'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { RailwaySimulator } = require('../src/railway/simulator');

test('RailwaySimulator exposes complete train/layout state', () => {
  const sim = new RailwaySimulator(); const state = sim.snapshot();
  assert.equal(state.system.mode, 'SIMULATION'); assert.equal(state.trains.length, 3);
  assert.equal(state.sections.length, 6); assert.equal(state.signals.length, 6); assert.equal(state.routes.length, 3);
});

test('RUN and HOLD change train state', () => {
  const sim = new RailwaySimulator(); let result = sim.command('RUN', { train: '1A34', speed: 30 });
  assert.equal(result.ok, true); assert.equal(result.result.state, 'RUNNING'); assert.equal(result.result.speedKph, 30);
  result = sim.command('HOLD', { train: '1A34' }); assert.equal(result.ok, true); assert.equal(result.result.state, 'HELD'); assert.equal(result.result.speedKph, 0);
});

test('emergency stop blocks movement until reset', () => {
  const sim = new RailwaySimulator(); assert.equal(sim.command('ESTOP').ok, true);
  assert.equal(sim.command('RUN', { train: '1A34' }).ok, false); assert.equal(sim.snapshot().system.emergency, true);
  assert.equal(sim.command('RESET_ESTOP').ok, true); assert.equal(sim.snapshot().system.emergency, false);
});

test('unknown train is rejected', () => { const sim = new RailwaySimulator(); assert.equal(sim.command('RUN', { train: 'ZZ99' }).ok, false); });
