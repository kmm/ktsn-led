import { describe, expect, it } from 'vitest';
import { boardIoModes, versionAtLeast } from './types';

describe('versionAtLeast', () => {
  it('compares major, minor and patch as numbers', () => {
    expect(versionAtLeast('0.20.0', '0.20.0')).toBe(true);
    expect(versionAtLeast('0.23.1', '0.20.0')).toBe(true);
    expect(versionAtLeast('1.0.0', '0.21.0')).toBe(true);
    expect(versionAtLeast('0.19.9', '0.20.0')).toBe(false);
    expect(versionAtLeast('0.9.0', '0.10.0')).toBe(false);
    expect(versionAtLeast('0.20.0-dirty', '0.20.0')).toBe(true);
    expect(versionAtLeast(undefined, '0.20.0')).toBe(false);
    expect(versionAtLeast('v6.1', '0.20.0')).toBe(false);
  });
});

describe('boardIoModes', () => {
  it('offers inputs and audio by firmware version, also with no slots yet', () => {
    expect(boardIoModes('0.23.0', [])).toEqual(['binary', 'pwm', 'input', 'analog', 'audio']);
    expect(boardIoModes('0.20.0', [])).toEqual(['binary', 'pwm', 'input', 'analog']);
    expect(boardIoModes('0.19.0', [])).toEqual(['binary', 'pwm']);
  });
  it('falls back to what the slots show without a version', () => {
    expect(boardIoModes(undefined, [])).toEqual(['binary', 'pwm']);
    expect(boardIoModes(undefined, [{ mode: 'binary', pull: 'up' }])).toEqual(['binary', 'pwm', 'input', 'analog']);
    expect(boardIoModes(undefined, [{ mode: 'analog' }])).toEqual(['binary', 'pwm', 'input', 'analog']);
    expect(boardIoModes(undefined, [{ mode: 'audio' }])).toEqual(['binary', 'pwm', 'input', 'analog', 'audio']);
    expect(boardIoModes(undefined, [{ mode: 'pwm' }])).toEqual(['binary', 'pwm']);
  });
});
