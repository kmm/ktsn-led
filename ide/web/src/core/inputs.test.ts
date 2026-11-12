import { describe, expect, it } from 'vitest';
import { boardInputs, inputsInSource } from './inputs';

describe('preview inputs', () => {
  it('finds the inputs a program reads, once each, skipping comments and strings', () => {
    const src = '(if (in button) (fill (buf O) #FF0000))\n; (in commented)\n(str-cat "(in quoted)" "x")\n' +
      '(scale (buf O) (analog knob 64))\n(if (in button) (scale (buf O) (analog 3)))\n(in-thing)';
    expect(inputsInSource(src)).toEqual([
      { key: 'button', label: 'button', kind: 'digital' },
      { key: 'knob', label: 'knob', kind: 'analog' },
      { key: '3', label: 'slot 3', kind: 'analog' },
    ]);
  });
  it('lists a board\'s input slots, not its outputs', () => {
    expect(boardInputs([{ n: 0, name: 'pump', mode: 'binary' }, { n: 2, name: '', mode: 'input' }, { n: 5, name: 'knob', mode: 'analog' }]))
      .toEqual([{ key: '2', label: 'slot 2', kind: 'digital' }, { key: '5', label: 'knob', kind: 'analog' }]);
  });
});
