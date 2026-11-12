// The KTSN LED language reference: one entry per function (and `def`), plus topic pages for the shared rules.
// Tests check that every function the compiler knows has an entry, that argument counts match the compiler's table,
// and that every example compiles. Keep the wording in sync with the firmware's behaviour (CLAUDE.md "DSL conventions").

export type ArgType = 'buffer' | 'color' | 'int' | 'number' | 'number or color' | '0..1' | 'boolean' | 'name' | 'value' | 'string' | 'int or string' | 'string or number' | 'function' | 'parameters' | 'body';

export interface ArgSpec {
  name: string;
  type: ArgType;
  desc: string;
  optional?: boolean;
  def?: string;               // default when omitted or `_`
  many?: number;              // this argument may repeat up to `many` times in total
}

export interface Entry {
  name: string;
  category: Category;
  summary: string;            // one line, shown on hover
  args: ArgSpec[];
  returns: string;
  details: string;            // a few short paragraphs, separated by blank lines
  example: string;            // a complete program; must compile
  context?: 'automation';     // only in the automation program (the example compiles and runs as one)
}

export type Category = 'Buffers' | 'Filling' | 'Pixels' | 'Blending' | 'Motion' | 'Logic' | 'Math' | 'Environment' | 'Time' | 'Variables' | 'Functions' | 'Text' | 'Displays' | 'Automation';
export const CATEGORIES: Category[] = ['Buffers', 'Filling', 'Pixels', 'Blending', 'Motion', 'Logic', 'Math', 'Environment', 'Time', 'Variables', 'Functions', 'Text', 'Displays', 'Automation'];
/** Words the compiler handles itself (not natives): documented, but not in the compiler's native table. */
export const KEYWORDS = ['def', 'defn', 'fn', 'use'];

const range: ArgSpec[] = [
  { name: 'start', type: 'int', desc: 'first pixel of the range; negative counts from the end', optional: true, def: '0' },
  { name: 'end', type: 'int', desc: 'pixel after the last one (exclusive); negative counts from the end', optional: true, def: 'the length' },
];
const dstSrc = (verb: string): ArgSpec[] => [
  { name: 'dst', type: 'buffer', desc: `buffer (or slice) to ${verb} into` },
  { name: 'src', type: 'buffer', desc: 'buffer (or slice) to read from' },
];
const blendDetails = (formula: string) =>
  `${formula}\n\nPixel i of dst is combined with pixel i of src, over the shorter of the two. Use slice to position or ` +
  'reverse either side. dst and src may overlap: the result is as if src were read completely first.';

const TIME_RULES = 'A time is a string "HH:MM" or "HH:MM:SS" (the hour may be one digit), or a number of minutes ' +
  'after midnight, so (env sunset), (env sunrise) and (env daymin) fit straight in. An int counts to the minute, a ' +
  'decimal to the second (1110.25 is 18:30:15); numbers wrap at 24 hours, and a negative one, such as sunset without ' +
  'a location, makes the test false.\n\nThe comparison works at the precision the time is written in: "18:30" is the ' +
  'whole minute, so (time-eq "18:30") is true for 60 seconds, (time-gt "18:30") only from 18:31:00, and gt, eq and ' +
  'lt never overlap. "18:30:15" compares to the second.\n\nAll time and date tests are false while the clock isn\'t ' +
  'set ((env timeset) is 0), so nothing switches on by mistake right after power-up. A time written in the program ' +
  'is checked when it compiles; one made while it runs (str-cat) is checked when used, and a bad one is a fault.';
const DATE_RULES = 'A date is a string "MM-DD", which matches every year, or "YYYY-MM-DD", one day. "MM-DD" ' +
  'comparisons stay within the year: (date-lt "03-01") is true in January and February. The date is the board\'s local ' +
  'date. All time and date tests are false while the clock isn\'t set ((env timeset) is 0). A date written in the ' +
  'program is checked when it compiles (02-29 is fine for "MM-DD"; with a year it must be a leap year).';

function timeCmp(name: string, word: string, summary: string): Entry[] {
  return [{
    name, category: 'Time', summary,
    args: [{ name: 'time', type: 'string or number', desc: '"HH:MM", "HH:MM:SS" or minutes after midnight' }],
    returns: 'boolean',
    details: `True when the local time of day is ${word} the given time.\n\n${TIME_RULES}`,
    example: `; on ${word} 18:30 (to the minute), off otherwise\n(if (${name} "18:30")\n  (fill (buf O) #FFA040)\n  (fill (buf O) #000000))\n` +
      `; the same with a number: minutes after midnight\n(if (${name} (+ (* 18 60) 30)) (scale (buf O) 0.5))`,
  }];
}
function dateCmp(name: string, word: string, summary: string): Entry[] {
  return [{
    name, category: 'Time', summary,
    args: [{ name: 'date', type: 'string', desc: '"MM-DD" (every year) or "YYYY-MM-DD"' }],
    returns: 'boolean',
    details: `True when today is ${word} the given date.\n\n${DATE_RULES}`,
    example: `; orange ${word} Halloween, every year\n(if (${name} "10-31")\n  (fill (buf O) #FF6000)\n  (fill (buf O) #000000))`,
  }];
}

export const ENTRIES: Entry[] = [
  /* ---------------- Buffers ---------------- */
  {
    name: 'buf', category: 'Buffers', summary: 'One of the channel\'s buffers X Y Z O, or a global buffer G0..G7.',
    args: [{ name: 'id', type: 'name', desc: 'X, Y, Z, O (this channel, one pixel per LED) or G0..G7 (global, 256 pixels each)' }],
    returns: 'buffer',
    details: 'O is what the string shows: after every frame, O is sent to the LEDs. X, Y and Z are scratch buffers of the same ' +
      'length. All four keep their contents from one frame to the next.\n\nG0..G7 are shared by every channel and by the ' +
      'autoexec program: a good place for palettes. Lower-case ids work too.',
    example: '(fill (buf X) #0000FF)\n(fill (buf O) #000000)\n(copy (buf O) (buf X) 0 10)    ; first 10 pixels from X',
  },
  {
    name: 'slice', category: 'Buffers', summary: 'A view of part of a buffer: a range, optionally every n-th pixel or reversed.',
    args: [
      { name: 'buf', type: 'buffer', desc: 'buffer or slice to take the view from' },
      ...range,
      { name: 'step', type: 'int', desc: 'take every step-th pixel; negative takes the same pixels in reverse order; 0 is an error', optional: true, def: '1' },
    ],
    returns: 'buffer (a view: writing to it writes the underlying pixels)',
    details: 'A slice doesn\'t copy anything: it is a window onto the buffer, and every function that takes a buffer works on ' +
      'it. Indices inside a slice count from the slice\'s own start.\n\n(slice b 0 10 2) is pixels 0 2 4 6 8; with step -2 it ' +
      'is 8 6 4 2 0, the same pixels reversed. (slice b _ _ -1) reverses the whole buffer. Slices of slices combine.',
    example: '(fill (buf O) #000000)\n(fill (slice (buf O) 0 _ 2) #FF0000)       ; every other pixel\n(fill (slice (buf O) -10) #0000FF)         ; the last ten',
  },

  /* ---------------- Filling ---------------- */
  {
    name: 'fill', category: 'Filling', summary: 'Set every pixel of a range to one color.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice' }, { name: 'color', type: 'color', desc: '#RRGGBB or #RRGGBBWW' }, ...range],
    returns: 'buf',
    details: 'The most basic building block. Returns its buffer, so calls nest: (scale (fill (buf O) #FF8000) 0.5).',
    example: '(fill (buf O) #000000)\n(fill (buf O) #00FF00 5 -5)    ; all but 5 pixels at each end',
  },
  {
    name: 'scale', category: 'Filling', summary: 'Multiply the brightness of a range by an amount from 0 to 1.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice' }, { name: 'amount', type: '0..1', desc: '0 is black, 1 leaves the pixels unchanged; clamped to 0..1' }, ...range],
    returns: 'buf',
    details: 'Scales all channels (R, G, B, W) of each pixel, rounding down. Scaling the same persistent buffer every frame ' +
      'compounds: fill it first in the same program, or scale a copy. That compounding also makes trails: ' +
      '(scale (buf O) 0.9) each frame fades every pixel all the way to black.',
    example: '(fill (buf O) #FF8000)\n(scale (buf O) (+ 0.55 (* 0.45 (sin (* (env frame) 2)))))   ; breathing',
  },
  {
    name: 'invert', category: 'Filling', summary: 'Invert every channel of a range: each value x becomes 255 - x.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice' }, ...range],
    returns: 'buf',
    details: 'Inverts all four channels (R, G, B and W), so black becomes white and a segment mask from seg7, seg9 or seg14 ' +
      'turns into the unlit segments. On an RGBW strip an inverted color also lights the white LEDs; use it on masks ' +
      'and multiply the result with a color.',
    example: '(seg7 (buf X) (env second))\n(invert (buf X))\n(fill (buf O) #0040FF)\n(multiply (buf O) (buf X))   ; the unlit segments in blue',
  },
  {
    name: 'fill-pat', category: 'Filling', summary: 'Repeat a pattern of pixels across a range, optionally mirrored, stretched or shifted.',
    args: [
      { name: 'dst', type: 'buffer', desc: 'buffer or slice to fill' },
      { name: 'pattern', type: 'buffer', desc: 'buffer or slice holding the pattern\'s pixels' },
      { name: 'phase', type: 'int', desc: 'shift the pattern; positive moves it right (like rotate)', optional: true, def: '0' },
      { name: 'stretch', type: 'int', desc: 'repeat each pattern pixel this many times (at least 1)', optional: true, def: '1' },
      { name: 'mirror', type: 'boolean', desc: 't plays the pattern forward then backward: ABC becomes ABCCBA', optional: true, def: 'nil' },
      ...range,
    ],
    returns: 'dst',
    details: 'The sequence is built as: pattern, mirrored if asked, then each pixel repeated `stretch` times; then shifted by ' +
      '`phase` and repeated across the range, starting at the range\'s first pixel.\n\nUsing (env frame) as the phase marches ' +
      'the pattern along without any blurring. An empty pattern does nothing. Reverse or space out the pattern with slice.',
    example: '(def pal (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF))\n(fill-pat (buf O) pal (/ (env frame) 4) 3 t)',
  },
  {
    name: 'fill-grad', category: 'Filling', summary: 'Fill a range with a smooth gradient through a palette\'s colors.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice to fill' }, { name: 'palette', type: 'buffer', desc: 'buffer or slice holding the colors, in order' }, ...range],
    returns: 'buf',
    details: 'The palette\'s colors are spread evenly over the range, blending linearly in between. The first and last ' +
      'colors land exactly on the range\'s first and last pixels. A palette with no pixels does nothing; one color fills.',
    example: '(fill-grad (buf O) (set (slice (buf G0) 0 3) 0 #FF0000 #FFFF00 #0000FF))',
  },
  {
    name: 'fill-rand', category: 'Filling', summary: 'Fill a range with one color at a random brightness per pixel, new every frame.',
    args: [
      { name: 'buf', type: 'buffer', desc: 'buffer or slice' },
      { name: 'color', type: 'color', desc: 'the color at full brightness' },
      { name: 'min', type: '0..1', desc: 'lowest brightness' },
      { name: 'max', type: '0..1', desc: 'highest brightness (min and max may be swapped)' },
      ...range,
    ],
    returns: 'buf',
    details: 'Each pixel gets the color scaled by a random amount between min and max, chosen again every frame (60 times a ' +
      'second), which shimmers. Each channel has its own random sequence.',
    example: '(fill-rand (buf O) #FF6000 0.3 1)    ; flickering embers',
  },
  {
    name: 'fill-pick', category: 'Filling', summary: 'Per pixel, choose between two colors at random, new every frame.',
    args: [
      { name: 'buf', type: 'buffer', desc: 'buffer or slice' },
      { name: 'color1', type: 'color', desc: 'color used with probability 1 - p' },
      { name: 'color2', type: 'color', desc: 'color used with probability p' },
      { name: 'p', type: '0..1', desc: 'chance of color2: 0 never, 1 always' },
      ...range,
    ],
    returns: 'buf',
    details: 'Chosen again every frame, so a small p with a bright color2 gives sparkles.',
    example: '(fill-pick (buf O) #000010 #FFFFFF 0.03)    ; sparkles on deep blue',
  },

  /* ---------------- Pixels ---------------- */
  {
    name: 'set', category: 'Pixels', summary: 'Write one or more colors to consecutive pixels, starting at an index.',
    args: [
      { name: 'buf', type: 'buffer', desc: 'buffer or slice' },
      { name: 'index', type: 'int', desc: 'where the first color goes; negative counts from the end' },
      { name: 'color', type: 'color', desc: 'colors written to index, index+1, …', many: 8 },
    ],
    returns: 'buf',
    details: 'Pixels that fall outside the buffer are skipped (not clamped), so a dot moving off an edge disappears. Returns ' +
      'its buffer, which makes it handy for building palettes inline: (set (slice (buf G0) 0 3) 0 #F00 …).',
    example: '(fill (buf O) #000000)\n(set (buf O) (mod (/ (env frame) 3) 50) #FFFFFF)    ; a dot moving along',
  },
  {
    name: 'get', category: 'Pixels', summary: 'The color of one pixel.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice' }, { name: 'index', type: 'int', desc: 'pixel to read; negative counts from the end' }],
    returns: 'color (black when the index is outside the buffer)',
    details: 'Usable anywhere a color goes, e.g. to copy one pixel or to compare it with eq.',
    example: '(set (buf G1) 0 #8000FF)\n(fill (buf O) (get (buf G1) 0))',
  },

  /* ---------------- Blending ---------------- */
  {
    name: 'copy', category: 'Blending', summary: 'Copy pixels from one buffer to another.',
    args: [...dstSrc('copy'), ...range], returns: 'dst',
    details: blendDetails('dst[i] = src[i].') + '\n\nBecause overlap is safe, (copy (slice (buf O) 1) (buf O)) shifts the ' +
      'buffer right by one pixel.',
    example: '(fill (buf X) #000000)\n(set (buf X) 0 #FF0000 #00FF00 #0000FF)\n(copy (slice (buf O) 20) (buf X) 0 3)    ; X\'s first 3 pixels at 20',
  },
  {
    name: 'screen', category: 'Blending', summary: 'Screen blend: brightens; black leaves dst alone, white saturates.',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: 255 - (255 - dst)(255 - src) / 255. Softer than add.'),
    example: '(fill (buf O) #000000) (fill (buf O) #FF0000 0 30)\n(fill (buf Y) #000000) (fill (buf Y) #0000FF 20 50)\n(screen (buf O) (buf Y))    ; magenta where they overlap',
  },
  {
    name: 'add', category: 'Blending', summary: 'Additive blend, like overlapping lights: per channel min(255, dst + src).',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: min(255, dst + src). Punchier than screen, clips sooner.'),
    example: '(fill (buf O) #400000)\n(fill (buf X) #000040)\n(add (buf O) (buf X))',
  },
  {
    name: 'subtract', category: 'Blending', summary: 'Subtractive blend: per channel max(0, dst - src); src darkens dst.',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: max(0, dst - src). Black src changes nothing, white src clears dst to black; a ' +
      'colored src removes that color (subtracting #00FFFF from white leaves red). The opposite of add.'),
    example: '(fill (buf O) #FFFFFF)\n(fill (buf X) #000000)\n(fill (buf X) #00FFFF (mod (/ (env frame) 3) 50) (+ 5 (mod (/ (env frame) 3) 50)))\n(subtract (buf O) (buf X))',
  },
  {
    name: 'lighten', category: 'Blending', summary: 'Keep the brighter of the two, per channel.',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: max(dst, src). Layering never gets brighter than the brightest input.'),
    example: '(fill (buf O) #804020)\n(fill (buf X) #20A010)\n(lighten (buf O) (buf X))',
  },
  {
    name: 'darken', category: 'Blending', summary: 'Keep the darker of the two, per channel.',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: min(dst, src).'),
    example: '(fill (buf O) #FFFFFF)\n(fill-grad (buf X) (set (slice (buf G0) 0 2) 0 #FF0000 #0000FF))\n(darken (buf O) (buf X))',
  },
  {
    name: 'multiply', category: 'Blending', summary: 'Multiply blend: white leaves dst alone, black blanks it. Good for masks.',
    args: [...dstSrc('blend'), ...range], returns: 'dst',
    details: blendDetails('Per channel: dst × src / 255.'),
    example: '(fill (buf O) #FF8000)\n(fill-grad (buf X) (set (slice (buf G0) 0 3) 0 #000000 #FFFFFF #000000))\n(multiply (buf O) (buf X))    ; fade at both ends',
  },
  {
    name: 'mix', category: 'Blending', summary: 'Crossfade from dst toward src by an amount from 0 to 1.',
    args: [...dstSrc('blend'), { name: 'amount', type: '0..1', desc: '0 keeps dst, 1 is src' }, ...range], returns: 'dst',
    details: blendDetails('Per channel: dst + (src - dst) × amount, rounded, but always at least one step toward src when ' +
      'amount is above 0, so mixing into the same buffer every frame reaches src exactly.'),
    example: '(fill (buf O) #FF0000)\n(fill (buf X) #0000FF)\n(mix (buf O) (buf X) (+ 0.5 (* 0.5 (sin (env frame)))))',
  },
  {
    name: 'grad-map', category: 'Blending', summary: 'Recolor by brightness: each src pixel\'s luma picks a color along a palette.',
    args: [
      { name: 'dst', type: 'buffer', desc: 'buffer or slice written' },
      { name: 'src', type: 'buffer', desc: 'buffer or slice whose brightness is read (may be dst itself)' },
      { name: 'palette', type: 'buffer', desc: 'buffer or slice holding the colors, darkest-input first' },
      ...range,
    ],
    returns: 'dst',
    details: 'Luma = (77·R + 150·G + 29·B) / 256, 0..255 (the white byte is ignored). The palette\'s colors are spread ' +
      'evenly over luma 0..255, the first at black and the last at white, blending linearly in between. dst\'s old value ' +
      'is replaced. Like the blends, the range runs over the shorter of dst and src. A palette with no pixels does ' +
      'nothing; one color fills. A palette inside dst is read before anything is written.',
    example: '(fill-grad (buf X) (set (slice (buf G0) 0 2) 0 #000000 #FFFFFF))   ; a grey ramp\n' +
      '(rotate (buf X) (mod (env frame) 50))\n' +
      '(grad-map (buf O) (buf X) (set (slice (buf G1) 0 4) 0 #000020 #8000FF #FF4000 #FFFF80))',
  },

  /* ---------------- Motion ---------------- */
  {
    name: 'rotate', category: 'Motion', summary: 'Rotate a range\'s pixels right (positive) or left, wrapping around; fractions blend.',
    args: [{ name: 'buf', type: 'buffer', desc: 'buffer or slice' }, { name: 'amount', type: 'number', desc: 'pixels to rotate; positive moves right, negative left' }, ...range],
    returns: 'buf',
    details: 'Amounts within 0.01 of a whole number move whole pixels exactly. Other amounts move by the fraction too, ' +
      'blending neighbours, which softens the pattern: rotating the same buffer by a fraction every frame blurs it. For ' +
      'smooth sub-pixel motion, rotate a fresh copy each frame by an amount that grows with time.',
    example: '(fill-grad (buf X) (set (slice (buf G0) 0 3) 0 #FF0000 #00FF00 #0000FF))\n(copy (buf O) (buf X))\n(rotate (buf O) (* (mod (env frame) 200) 0.25))',
  },

  /* ---------------- Logic ---------------- */
  {
    name: 'if', category: 'Logic', summary: 'Run (or pick) one of two branches depending on a condition.',
    args: [
      { name: 'cond', type: 'boolean', desc: 'a boolean: t, nil, or a comparison' },
      { name: 'then', type: 'value', desc: 'used when cond is t' },
      { name: 'else', type: 'value', desc: 'used when cond is nil', optional: true },
    ],
    returns: 'the taken branch\'s value (nothing without an else)',
    details: 'Only the chosen branch runs. With both branches, if is a value of their common type: colors, buffers or ' +
      'numbers (an int and a decimal give a decimal). Branches of different kinds are a compile error. Without an else it ' +
      'is just a statement.',
    example: '(fill (buf O) (if (lt (env second) 30) #FF0000 #0000FF))',
  },
  {
    name: 'and', category: 'Logic', summary: 't if all conditions are t; stops at the first nil.',
    args: [{ name: 'a', type: 'boolean', desc: 'condition' }, { name: 'b', type: 'boolean', desc: 'more conditions', many: 9 }],
    returns: 'boolean',
    details: 'Evaluates left to right and stops as soon as one is nil, so later conditions may rely on earlier ones.',
    example: '(fill (buf O) #000000)\n(if (and (ge (env hour) 18) (lt (env hour) 23)) (fill (buf O) #FF8000))',
  },
  {
    name: 'or', category: 'Logic', summary: 't if any condition is t; stops at the first t.',
    args: [{ name: 'a', type: 'boolean', desc: 'condition' }, { name: 'b', type: 'boolean', desc: 'more conditions', many: 9 }],
    returns: 'boolean',
    details: 'Evaluates left to right and stops as soon as one is t.',
    example: '(fill (buf O) (if (or (lt (env hour) 7) (gt (env hour) 22)) #100008 #FFFFFF))',
  },
  {
    name: 'not', category: 'Logic', summary: 't for nil and nil for t.',
    args: [{ name: 'a', type: 'boolean', desc: 'condition' }], returns: 'boolean', details: 'Numbers are not booleans: compare them first.',
    example: '(if (not (eq (env second) 0)) (fill (buf O) #002000) (fill (buf O) #FFFFFF))',
  },
  ...(['lt', 'gt', 'le', 'ge'] as const).map((n): Entry => ({
    name: n, category: 'Logic',
    summary: { lt: 'a < b', gt: 'a > b', le: 'a ≤ b', ge: 'a ≥ b' }[n] + ' for numbers.',
    args: [{ name: 'a', type: 'number', desc: 'int or decimal' }, { name: 'b', type: 'number', desc: 'int or decimal' }],
    returns: 'boolean',
    details: 'Ints and decimals can be mixed. Large ints such as (env frame) compare exactly.',
    example: `(fill (buf O) (if (${n} (env second) 30) #00FF00 #FF0000))`,
  })),
  ...(['eq', 'ne'] as const).map((n): Entry => ({
    name: n, category: 'Logic',
    summary: n === 'eq' ? 'Whether two values are equal.' : 'Whether two values differ.',
    args: [{ name: 'a', type: 'value', desc: 'number, color, boolean or buffer' }, { name: 'b', type: 'value', desc: 'a value of the same kind' }],
    returns: 'boolean',
    details: 'Works on two numbers (ints and decimals mix: (eq 2 2.0) is t), two colors, two booleans, or two buffers ' +
      '(the same view of the same pixels). Comparing different kinds is a compile error.',
    example: `(set (buf G1) 0 #FF0000)\n(fill (buf O) (if (${n} (get (buf G1) 0) #FF0000) #00FF00 #000000))`,
  })),

  /* ---------------- Math ---------------- */
  ...([['+', 'Add numbers.', 2, 10], ['*', 'Multiply numbers.', 2, 10]] as const).map(([n, s]): Entry => ({
    name: n, category: 'Math', summary: s,
    args: [{ name: 'a', type: 'number', desc: 'int or decimal' }, { name: 'b', type: 'number', desc: 'more numbers', many: 9 }],
    returns: 'int if every argument is an int, otherwise decimal',
    details: 'Results saturate instead of wrapping around. Decimals are limited to about ±32767; ints have their full range.',
    example: `(fill (buf O) #000000)\n(fill (buf O) #FFFFFF 0 (${n} 2 (/ (env second) 10)))`,
  })),
  {
    name: '-', category: 'Math', summary: 'Subtract, or negate with one argument.',
    args: [{ name: 'a', type: 'number', desc: 'int or decimal' }, { name: 'b', type: 'number', desc: 'subtracted from a; without it, (- a) is -a', optional: true }],
    returns: 'int or decimal (decimal if either is)', details: 'Results saturate instead of wrapping around.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #FF0000 (- 50 (/ (env second) 2)))',
  },
  {
    name: '/', category: 'Math', summary: 'Divide. Two ints give an int, rounded down; otherwise a decimal.',
    args: [{ name: 'a', type: 'number', desc: 'dividend' }, { name: 'b', type: 'number', desc: 'divisor (not 0)' }],
    returns: 'int (two ints, rounded down) or decimal',
    details: '(/ 7 2) is 3 and (/ -7 2) is -4; (/ 7 2.0) is 3.5. Dividing by zero is a compile error when constant and a ' +
      'fault (the frame stops) when computed.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #00FF00 0 (/ (env second) 6))    ; 0..9 pixels',
  },
  {
    name: 'mod', category: 'Math', summary: 'Remainder with the divisor\'s sign: (mod -1 60) is 59. Works on decimals too.',
    args: [{ name: 'a', type: 'number', desc: 'dividend' }, { name: 'b', type: 'number', desc: 'divisor (not 0)' }],
    returns: 'int or decimal',
    details: 'Floor modulo: the result always has the sign of b, which is what you want for wrapping positions and angles. ' +
      '(mod 7.5 2) is 1.5.',
    example: '(fill (buf O) #000000)\n(set (buf O) (mod (env frame) 50) #FFFFFF)',
  },
  {
    name: 'abs', category: 'Math', summary: 'Absolute value.', args: [{ name: 'x', type: 'number', desc: 'int or decimal' }],
    returns: 'same kind as x', details: '',
    example: '(fill (buf O) #000000)\n(fill (buf O) #0080FF 0 (abs (- (mod (env frame) 100) 50)))    ; bounces 0..50',
  },
  ...([['min', 'The smallest of 2 to 10 numbers.'], ['max', 'The largest of 2 to 10 numbers.']] as const).map(([n, s]): Entry => ({
    name: n, category: 'Math', summary: s,
    args: [{ name: 'a', type: 'number', desc: 'int or decimal' }, { name: 'b', type: 'number', desc: 'more numbers', many: 9 }],
    returns: 'int if every argument is an int, otherwise decimal', details: '',
    example: `(fill (buf O) #000000)\n(fill (buf O) #FFFFFF 0 (${n} 20 (env second)))`,
  })),
  {
    name: 'clamp', category: 'Math', summary: 'Limit a number to the range lo..hi.',
    args: [{ name: 'x', type: 'number', desc: 'value' }, { name: 'lo', type: 'number', desc: 'lower limit' }, { name: 'hi', type: 'number', desc: 'upper limit (lo and hi may be swapped)' }],
    returns: 'int if every argument is an int, otherwise decimal', details: '',
    example: '(fill (buf O) #FFFFFF)\n(scale (buf O) (clamp (sin (* (env frame) 3)) 0.1 1))',
  },
  {
    name: 'floor', category: 'Math', summary: 'Round a decimal down to an int.', args: [{ name: 'x', type: 'number', desc: 'int or decimal' }],
    returns: 'int', details: 'Indices and ranges need ints; floor and round turn decimals into ints. (floor -2.5) is -3.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #FFFF00 0 (floor (* 25 (+ 1 (sin (env frame))))))',
  },
  {
    name: 'round', category: 'Math', summary: 'Round a decimal to the nearest int (halves round up).', args: [{ name: 'x', type: 'number', desc: 'int or decimal' }],
    returns: 'int', details: '(round 2.5) is 3 and (round -2.5) is -2.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #FFFF00 0 (round (* 25 (+ 1 (sin (env frame))))))',
  },
  ...([['sin', 'Sine of an angle in degrees, from -1 to 1.'], ['cos', 'Cosine of an angle in degrees, from -1 to 1.']] as const).map(([n, s]): Entry => ({
    name: n, category: 'Math', summary: s,
    args: [{ name: 'deg', type: 'number', desc: 'angle in degrees (any size, int or decimal)' }],
    returns: 'decimal -1..1',
    details: 'Table-based with 1 degree steps and interpolation in between: accurate to about 0.0001. Large ints such as ' +
      '(env frame) work exactly, so (sin (* (env frame) 3)) is a wave with a 2 s period at 60 frames per second.',
    example: `(fill (buf O) #00A0FF)\n(scale (buf O) (+ 0.5 (* 0.5 (${n} (* (env frame) 3)))))`,
  })),
  {
    name: 'tri', category: 'Math', summary: 'Triangle wave of an angle in degrees, from -1 to 1: like sin, but in straight lines.',
    args: [{ name: 'deg', type: 'number', desc: 'angle in degrees (any size, int or decimal)' }],
    returns: 'decimal -1..1',
    details: 'Same period and phase as sin: 0 at 0°, 1 at 90°, 0 at 180°, -1 at 270°, rising and falling at a constant rate in ' +
      'between, so it can replace sin wherever an even back-and-forth (a bouncing position, a linear pulse) looks better ' +
      'than a sine\'s ease in and out. Exact (no table), and large ints such as (env frame) work like with sin.',
    example: '(fill (buf O) #000000)\n(set (buf O) (round (* 24.5 (+ 1 (tri (* (env frame) 2))))) #FFFFFF)    ; bounce end to end',
  },
  {
    name: 'lerp', category: 'Math', summary: 'The value t of the way from a to b: numbers or colors.',
    args: [
      { name: 'a', type: 'number or color', desc: 'start (t = 0)' },
      { name: 'b', type: 'number or color', desc: 'end (t = 1); the same kind as a' },
      { name: 't', type: 'number', desc: 'position: 0 is a, 1 is b, 0.5 halfway' },
    ],
    returns: 'numbers: int if a, b and t are ints, otherwise decimal; colors: color',
    details: 'Numbers: a + (b - a) × t. t is not clamped, so values outside 0..1 continue the line (clamp t if that isn\'t ' +
      'wanted). Colors: each channel blends like mix, and t is clamped to 0..1. (lerp 0 49 t) is a decimal; use floor or ' +
      'round to make it an index.',
    example: '(def pos (+ 0.5 (* 0.5 (sin (env frame)))))\n(fill (buf O) (lerp #FF2000 #0040FF pos))\n(set (buf O) (round (lerp 0 49 pos)) #FFFFFF)',
  },
  {
    name: 'rand', category: 'Math', summary: 'A random number, new on every call.',
    args: [
      { name: 'a', type: 'number', desc: 'alone: the upper limit, giving 0..a; with b: the lower limit', optional: true, def: 'none: a decimal 0..1' },
      { name: 'b', type: 'number', desc: 'upper limit', optional: true },
    ],
    returns: 'int if every argument is an int, otherwise decimal; (rand) is a decimal',
    details: '(rand) is a decimal from 0 up to (not including) 1, (rand 10) an int 0..9, (rand 3 7) an int 3..6, ' +
      '(rand 0.5 1.5) a decimal from 0.5 up to 1.5. The upper limit is never returned (like a range end); the limits may ' +
      'be in either order, and equal limits give that value. Each call is a new number, every frame; each channel has ' +
      'its own random sequence. _ can\'t be used here.',
    example: '(scale (buf O) 0.9)    ; fade what is there\n(if (lt (rand) 0.2) (set (buf O) (rand 50) (lerp #FFA000 #FFFFFF (rand))))    ; sparkles',
  },

  /* ---------------- Text ---------------- */
  {
    name: 'str-num', category: 'Text', summary: 'A number as text, with a width, decimals and padding of your choice.',
    args: [
      { name: 'number', type: 'number', desc: 'an int or a decimal' },
      { name: 'width', type: 'int', desc: 'at least this many characters (0..32), padded on the left', optional: true, def: '0 (no padding)' },
      { name: 'decimals', type: 'int', desc: 'digits after the point (0..4); decimals are rounded', optional: true, def: '0 for ints, 2 for decimals' },
      { name: 'zero', type: 'boolean', desc: 't pads with zeros (after a minus sign) instead of blanks', optional: true, def: 'nil' },
    ],
    returns: 'string',
    details: '(str-num 7 2) is " 7", (str-num 7 2 _ t) is "07", (str-num 3.14159 _ 1) is "3.1", (str-num -2.5 _ 0) is "-3" ' +
      '(halves round away from zero). A number that is wider than width is never cut.\n\nThe text lasts for the current run ' +
      'of the program (strings made while running live in the channel\'s per-frame scratch memory).',
    example: '; seconds, two digits with a leading zero\n(seg7 (buf X) (str-num (env second) 2 _ t))\n(fill (buf O) #00C0FF)\n(multiply (buf O) (buf X))',
  },
  {
    name: 'str-time', category: 'Text', summary: 'The date and time as text, through a format like "%H:%M".',
    args: [{ name: 'format', type: 'string', desc: 'text with %-codes: %H %k %I %l %M %S %p %Y %y %m %d %e %j %%' }],
    returns: 'string',
    details: 'Codes: %H hour 00-23, %k hour with a blank instead of the leading zero, %I hour 01-12, %l the same with a ' +
      'blank, %M minute, %S second, %p AM or PM, %Y year, %y two-digit year, %m month 01-12, %d day 01-31, %e day with a ' +
      'blank, %j day of the year 001-366, %% a percent sign. Other characters are copied as they are.\n\nThe time is the ' +
      'board\'s local time (env). An unknown code in a format written in the program is a compile error.',
    example: '; a 4-digit clock without a colon: " 905" at 9:05\n(seg7 (buf X) (str-time "%k%M"))\n(fill (buf O) #FF8000)\n(multiply (buf O) (buf X))',
  },
  {
    name: 'str-cat', category: 'Text', summary: 'Join strings (and numbers) into one string.',
    args: [{ name: 'a', type: 'string or number', desc: 'a string, or a number (written as str-num writes it by default)' },
      { name: 'b', type: 'string or number', desc: 'more parts', many: 9 }],
    returns: 'string',
    details: 'Takes 2 to 10 parts. Numbers are written as str-num writes them without options: ints as they are, decimals ' +
      'with 2 digits after the point; use str-num for anything else. The result is cut at 255 characters.',
    example: '(def txt (str-cat (str-num (env hour) 2) "h" (str-num (env minute) 2 _ t)))\n(seg14 (buf X) txt)\n(fill (buf O) #FFFFFF)\n(multiply (buf O) (buf X))',
  },
  {
    name: 'str-sub', category: 'Text', summary: 'Part of a string, by character positions (like slice).',
    args: [
      { name: 's', type: 'string', desc: 'the text' },
      { name: 'start', type: 'int', desc: 'first character; negative counts from the end', optional: true, def: '0' },
      { name: 'end', type: 'int', desc: 'character after the last one (exclusive); negative counts from the end', optional: true, def: 'the length' },
    ],
    returns: 'string',
    details: 'The same range rules as everywhere else: (str-sub "hello" 1 3) is "el", (str-sub "hello" -3) is "llo", ' +
      'and positions outside the text are clamped. It copies nothing. With str-len and env frame it scrolls text.',
    example: '; scroll a message across 3 digits, one step every half second\n(def msg "   HELLO   ")\n' +
      '(def i (mod (/ (env frame) 30) (- (str-len msg) 2)))\n(seg14 (buf X) (str-sub msg i (+ i 3)))\n(fill (buf O) #40FF40)\n(multiply (buf O) (buf X))',
  },
  {
    name: 'str-len', category: 'Text', summary: 'How many characters a string has.',
    args: [{ name: 's', type: 'string', desc: 'the text' }],
    returns: 'int',
    details: 'Counts characters (bytes). Strings are at most 255 characters.',
    example: '(def msg "HELLO")\n(fill (buf O) #000000)\n(fill (buf O) #FFFFFF 0 (str-len msg))',
  },

  /* ---------------- Displays ---------------- */
  {
    name: 'seg7', category: 'Displays', summary: 'A mask of a number or text on 7-segment digits wired as one strip.',
    args: [
      { name: 'dst', type: 'buffer', desc: 'buffer or slice the digits are wired as (the mask is written here)' },
      { name: 'value', type: 'int or string', desc: 'a number (right-aligned) or text (left-aligned)' },
      { name: 'layout', type: 'name', desc: 'the order the strip passes through the segments, each of a..g once (p = decimal point)', optional: true, def: 'abcdefg' },
      { name: 'hstride', type: 'int', desc: 'pixels in each horizontal segment (a, d, g)', optional: true, def: '1' },
      { name: 'vstride', type: 'int', desc: 'pixels in each vertical segment (b, c, e, f)', optional: true, def: '1' },
      { name: 'digits', type: 'int', desc: 'how many digits', optional: true, def: 'as many as fit' },
    ],
    returns: 'dst',
    details: 'Segments are named the usual way: a is the top, then clockwise b c d e f, and g the middle (p the decimal ' +
      'point, 1 pixel). The layout says in which order your strip runs through them, so any wiring works; each digit is ' +
      'those segments one after another, and digits follow each other from the start of dst (the first is the leftmost). ' +
      'Lit segments become #FFFFFFFF, the others black, so the mask works with any blend: (multiply (buf O) (buf X)) ' +
      'colours the digits with whatever O holds. Pixels after the last digit are left alone.\n\nA number is right-aligned ' +
      'with blanks; one that doesn\'t fit shows dashes. Text (a string, e.g. from str-num or str-time) is left-aligned and ' +
      'cut off at the last digit; a . or , after a character lights that digit\'s point instead of taking a digit. ' +
      'Characters a 7-segment digit can\'t show (K M W X …) are blank.',
    example: '; 4 digits, 3 px across and 2 px down per segment (14 px per digit) -> a clock\n' +
      '(seg7 (buf X) (+ (* (env hour) 100) (env minute)) abcdefg 3 2 4)\n' +
      '(fill-grad (buf O) (set (slice (buf G0) 0 2) 0 #FF3000 #FFB000))\n(multiply (buf O) (buf X))',
  },
  {
    name: 'seg9', category: 'Displays', summary: 'Like seg7, for 9-segment digits: 7 segments plus two centre verticals.',
    args: [
      { name: 'dst', type: 'buffer', desc: 'buffer or slice the digits are wired as (the mask is written here)' },
      { name: 'value', type: 'int or string', desc: 'a number (right-aligned) or text (left-aligned)' },
      { name: 'layout', type: 'name', desc: 'the order of the segments along the strip, each of a..g j m once (p = decimal point)', optional: true, def: 'abcdefgjm' },
      { name: 'hstride', type: 'int', desc: 'pixels in each horizontal segment (a, d, g)', optional: true, def: '1' },
      { name: 'vstride', type: 'int', desc: 'pixels in each vertical segment (b, c, e, f, j, m)', optional: true, def: '1' },
      { name: 'digits', type: 'int', desc: 'how many digits', optional: true, def: 'as many as fit' },
    ],
    returns: 'dst',
    details: 'a..g as on a 7-segment digit, plus j, the upper centre vertical (between f and b), and m, the lower one ' +
      '(between e and c); the names are those of a 14-segment display\'s centre verticals. They let the digit show letters ' +
      'a 7-segment digit can\'t, such as T I M W Y and a clearer B D R Q. 9-segment displays aren\'t a standard, so the ' +
      'character set is KTSN LED\'s own approximation; anything it doesn\'t improve looks as on seg7.\n\nEverything else ' +
      '(numbers and text, points, masks) works as in seg7.',
    example: '(seg9 (buf X) "TIME")\n(fill (buf O) #FFFFFF)\n(multiply (buf O) (buf X))',
  },
  {
    name: 'seg14', category: 'Displays', summary: 'A mask of text on 14-segment ("starburst") digits wired as one strip.',
    args: [
      { name: 'dst', type: 'buffer', desc: 'buffer or slice the digits are wired as (the mask is written here)' },
      { name: 'value', type: 'int or string', desc: 'a number (right-aligned) or text (left-aligned)' },
      { name: 'layout', type: 'name', desc: 'the order of the segments along the strip, each of a b c d e f g1 g2 h j k l m n once (p = point)', optional: true, def: 'abcdefg1g2hjklmn' },
      { name: 'hstride', type: 'int', desc: 'pixels in the top and bottom segments (a, d)', optional: true, def: '1' },
      { name: 'vstride', type: 'int', desc: 'pixels in each vertical segment (b, c, e, f, j, m)', optional: true, def: '1' },
      { name: 'dstride', type: 'int', desc: 'pixels in each diagonal (h, k, l, n)', optional: true, def: 'vstride' },
      { name: 'gstride', type: 'int', desc: 'pixels in each half of the middle bar (g1, g2)', optional: true, def: 'half of hstride, rounded up' },
      { name: 'digits', type: 'int', desc: 'how many digits', optional: true, def: 'as many as fit' },
    ],
    returns: 'dst',
    details: 'Segment names as on datasheets: a top, b c on the right, d bottom, e f on the left, g1 and g2 the left and ' +
      'right halves of the middle bar, h j k the upper diagonal, centre vertical and diagonal (left to right), n m l the ' +
      'lower ones (left to right), p the decimal point. Write the layout as the names run together in wiring order, e.g. ' +
      'abcdefg1g2hjklmn (upper case works too).\n\nShows upper-case letters, digits and most ASCII symbols; lower case is ' +
      'shown as upper case. Numbers, text, points and masks work as in seg7.',
    example: '; 3 digits of 14 px\n(seg14 (buf X) "HI!")\n(fill (buf O) #FF40A0)\n(multiply (buf O) (buf X))',
  },

  /* ---------------- Automation (only in the automation program) ---------------- */
  {
    name: 'play', category: 'Automation', context: 'automation', summary: 'Run a library program on a channel.',
    args: [{ name: 'ch', type: 'int', desc: 'channel number' }, { name: 'name', type: 'name', desc: 'a program in the board\'s library (lib)' }],
    returns: 'nothing',
    details: 'Only in the automation program. The name is checked when the automation program is compiled, so a typo or ' +
      'a program not in the library is a compile error. If the channel already runs that program (and nobody changed ' +
      'it), nothing happens, so calling play every few seconds is cheap. If the library program was deleted since, ' +
      'the call counts as a fault and the channel keeps what it has.',
    example: '(if (and (ge (env hour) 17) (lt (env hour) 23))\n    (play 0 evening)\n    (stop 0))',
  },
  {
    name: 'stop', category: 'Automation', context: 'automation', summary: 'Turn a channel off (its program is replaced by black).',
    args: [{ name: 'ch', type: 'int', desc: 'channel number' }],
    returns: 'nothing',
    details: 'Only in the automation program. The channel then runs (fill (buf O) #000000). Like play, nothing happens if ' +
      'the channel is already stopped. To dim instead of switching off, use set-intensity with a fade.',
    example: '(if (lt (env hour) 6) (stop 0))',
  },
  {
    name: 'io-set', category: 'Automation', context: 'automation', summary: 'Switch a GPIO output (relay) on or off, or set a PWM level.',
    args: [{ name: 'out', type: 'int', desc: 'output number, or its name (set with io config <n> ... name=<name>)' },
      { name: 'value', type: 'value', desc: 't / nil to switch it, or a level 0..1 (PWM duty; on a binary output anything above 0 is on)' }],
    returns: 'nothing',
    details: 'Only in the automation program. Outputs are configured on the board (io config, or the Board panel\'s Outputs) ' +
      'and are always off at boot; what the program sets is not saved.\n\nA name is looked up when the program compiles, so a ' +
      'misspelt or unknown output is a compile error. A name that is also a (def ...) variable means the variable. The ' +
      'automation program runs every few seconds: setting an output to what it already is changes nothing.',
    example: '(io-set 0 (and (ge (env hour) 7) (lt (env hour) 22)))\n(io-set 1 (if (ge (env hour) 18) 0.3 0))',
  },
  {
    name: 'io', category: 'Automation', context: 'automation', summary: 'Where a GPIO output is now, 0 to 1.',
    args: [{ name: 'out', type: 'int', desc: 'output number, or its name' }],
    returns: 'decimal 0..1 (binary outputs: 0 or 1)',
    details: 'Only in the automation program. Reads back the last value set, by io-set, the console (io) or the API.',
    example: '(if (eq (io 0) 0) (io-set 1 0.5) (io-set 1 0))',
  },
  {
    name: 'intensity', category: 'Automation', context: 'automation', summary: 'The global intensity, or a channel\'s, from 0 to 1.',
    args: [{ name: 'ch', type: 'int', desc: 'channel number; leave out for the global intensity', optional: true, def: 'the global intensity' }],
    returns: 'decimal 0..1',
    details: 'Only in the automation program. Mid-fade it is the value right now. The output uses global × channel. ' +
      'Render programs read the global value as (env intensity), 0..256.',
    example: '(if (gt (intensity 0) 0.5) (set-intensity 0 0.5 30))',
  },
  {
    name: 'set-intensity', category: 'Automation', context: 'automation', summary: 'Set a channel\'s (or the global) intensity, optionally fading.',
    args: [
      { name: 'ch', type: 'int', desc: 'channel number, or _ for the global intensity' },
      { name: 'value', type: '0..1', desc: 'new intensity' },
      { name: 'fade', type: 'number', desc: 'seconds to get there (fades smoothly, frame by frame)', optional: true, def: '0 (at once)' },
    ],
    returns: 'nothing',
    details: 'Only in the automation program. The fade runs on the board every frame, so an automation program that runs ' +
      'every few seconds can still dim smoothly; setting the same target again does not restart it. Up to 3600 s. ' +
      '`save` keeps the target values.',
    example: '; dim everything to 30 % over ten minutes from 22:00\n(if (ge (env hour) 22) (set-intensity _ 0.3 600) (set-intensity _ 1 60))',
  },

  /* ---------------- Environment ---------------- */
  {
    name: 'env', category: 'Environment', summary: 'Frame counter, local time and date, the global intensity, the channel length, or the sun and moon.',
    args: [{ name: 'name', type: 'name', desc: 'frame, hour, minute, second, ms, year, month, day, doy, weekday, daymin, daysec, ' +
      'dayfrac, hourfrac, minfrac, timeset, intensity, npix, sunrise, sunset, dawn, dusk, daylight, moon or moonlit' }],
    returns: 'int (a decimal for dayfrac, hourfrac and minfrac)',
    details: 'frame counts frames (60 a second). hour 0..23, minute and second 0..59, ms 0..999 within the second, year, ' +
      'month 1..12, day 1..31, doy (day of the year) 1..366, weekday 0..6 (0 Sunday), intensity 0..256, npix the number of ' +
      'pixels of the channel the program runs on (so one program fits strings of any length; in the preview: its pixel ' +
      'count; in the automation program: 1). Time is local (the board\'s time zone, from NTP or env time); in the preview ' +
      'it is your computer\'s clock or the simulated one. timeset is 1 once the clock has been set, 0 before (the time ' +
      'then counts from 1970). A misspelled name is a compile error that suggests the closest name.\n\nTime of day, ' +
      'by the clock: daymin is minutes since midnight 0..1439 (the same scale as sunrise and sunset), daysec seconds ' +
      '0..86399. dayfrac, hourfrac and minfrac are how far through the day, the hour and the minute it is, as decimals ' +
      'from 0.0 up to just below 1.0, including the milliseconds: multiply by a length or by 360 for a clock hand or a ' +
      'sweep. Decimals have steps of 1/65536, so dayfrac moves about every 1.3 s.\n\nsunrise, sunset, dawn and dusk (civil ' +
      'twilight) are minutes after local midnight, so compare them with daymin. They need a location: the console ' +
      'command loc on a board, the Lat and Lon fields in the preview. They are -1 with no location, with the clock not ' +
      'set, or when it doesn\'t happen that day (midnight sun, polar night); test for that before comparing. daylight ' +
      'is 1 while the sun is up (above the sunrise altitude, checked every second, also through a midnight sun), 0 ' +
      'when it is down, -1 with no location or clock. moon is the phase angle 0..359 (0 new, 90 first quarter, 180 ' +
      'full, 270 last quarter; it can go straight into sin) and moonlit the percent of the disc lit; they need only ' +
      'the clock, and are -1 until it is set.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #FF0000 0 (env second))    ; a bar growing every second\n\n' +
      '; a dot going round the string once a minute\n(set (buf O) (floor (* (env minfrac) (env npix))) #0000FF)\n\n' +
      '; lit while the sun is down, on weekdays\n(if (and (eq (env daylight) 0) (gt (env weekday) 0) (lt (env weekday) 6))\n  (fill (buf O) #402000))',
  },

  /* ---------------- Time and date ---------------- */
  ...timeCmp('time-gt', 'after', 'Later than a time of day (at the precision it is written in).'),
  ...timeCmp('time-ge', 'at or after', 'At or later than a time of day.'),
  ...timeCmp('time-lt', 'before', 'Earlier than a time of day.'),
  ...timeCmp('time-le', 'at or before', 'At or earlier than a time of day.'),
  ...timeCmp('time-eq', 'at', 'During that minute (or second) of the day.'),
  {
    name: 'time-in', category: 'Time', summary: 'Between two times of day; wraps past midnight.',
    args: [{ name: 'from', type: 'string or number', desc: 'start, included: "HH:MM", "HH:MM:SS" or minutes after midnight' },
      { name: 'to', type: 'string or number', desc: 'end, not included' }],
    returns: 'boolean',
    details: 'True from from up to (not including) to. When to is earlier than from the span goes past midnight: ' +
      '(time-in "22:00" "06:00") is true from 22:00 to 05:59:59. The same time twice is never true.\n\n' + TIME_RULES,
    example: '; warm white in the evening, until sunset if the location is set\n(if (time-in "17:00" "23:30")\n  (fill (buf O) #FF9040)\n  (fill (buf O) #000000))\n' +
      '(if (time-in (env sunset) "23:30") (scale (buf O) 0.5))',
  },
  ...dateCmp('date-gt', 'after', 'Later than a date.'),
  ...dateCmp('date-ge', 'on or after', 'On or later than a date.'),
  ...dateCmp('date-lt', 'before', 'Earlier than a date.'),
  ...dateCmp('date-le', 'on or before', 'On or earlier than a date.'),
  ...dateCmp('date-eq', 'on', 'On that day (every year, or one day).'),
  {
    name: 'date-in', category: 'Time', summary: 'Between two dates, both days included; "MM-DD" spans wrap past the year end.',
    args: [{ name: 'from', type: 'string', desc: 'first day, "MM-DD" or "YYYY-MM-DD"' },
      { name: 'to', type: 'string', desc: 'last day, written the same way' }],
    returns: 'boolean',
    details: 'True on from, to and every day between. With "MM-DD" dates the span repeats every year, and wraps past ' +
      'New Year when to comes before from: (date-in "12-01" "01-06") is December and the first six days of January. ' +
      'With "YYYY-MM-DD" dates it is that span once (nothing if to is before from). Both dates must be written the same ' +
      'way.\n\n' + DATE_RULES,
    example: '; red and green over the holidays\n(set (buf G0) 0 #FF0000 #00FF00)\n(if (date-in "12-01" "01-06")\n  (fill-pat (buf O) (slice (buf G0) 0 2))\n  (fill (buf O) #FFFFFF))',
  },

  /* ---------------- Variables ---------------- */
  {
    name: 'def', category: 'Variables', summary: 'Name a value (buffer, color, number or boolean) for the rest of the program.',
    args: [{ name: 'name', type: 'name', desc: 'a letter, then letters, digits, - or _ (up to 15); not t or nil' }, { name: 'value', type: 'value', desc: 'any value' }],
    returns: 'nothing',
    details: 'Only at the top level of a program or of a function body, never inside another call. The value is computed ' +
      'every frame; nothing carries over between frames. A name can be defined again, also from its own old value, ' +
      '(def n (+ n 1)), but keeps its kind: a color can\'t become a number.\n\nIn a function body, def makes a local ' +
      'variable, unless a variable of that name is already visible: then it is assigned. That is how a (fn …) passed to ' +
      'repeat adds up a total in a variable outside it. Up to 64 variables, parameters and locals per program.',
    example: '(def pal (set (slice (buf G0) 0 2) 0 #FF0000 #0000FF))\n(def w (* 2 (env second)))\n(fill (buf O) #000000)\n(fill-grad (slice (buf O) 0 (min 50 w)) pal)',
  },

  /* ---------------- Functions ---------------- */
  {
    name: 'defn', category: 'Functions', summary: 'Define a function: a name, typed parameters and a body, usable anywhere in the program.',
    args: [
      { name: 'name', type: 'name', desc: 'a letter, then letters, digits, - or _ (up to 15); not a built-in function' },
      { name: 'params', type: 'parameters', desc: '(name:type …), up to 10; types int decimal color bool buf str' },
      { name: 'body', type: 'body', desc: 'one or more forms; the last one\'s value is the result', many: 2 },
    ],
    returns: 'nothing (calling the function gives its body\'s value)',
    details: 'Only at the top level. A function can be called before or after its defn, from the program and from other ' +
      'functions: (dim #FF8000 0.5). Calls are checked against the parameters when the program compiles; an int passed to a ' +
      'decimal parameter becomes a decimal. The result type comes from the last form; a function whose last form returns ' +
      'nothing is a statement.\n\nA function sees only its parameters, its own locals (def in the body), env and the ' +
      'buffers, never the program\'s variables: pass in what it needs.\n\nFunctions can\'t call each other in a circle. A ' +
      'function may call itself as its very last step (a tail call, also inside the branches of a final if): that runs the ' +
      'body again with the new parameters, like a loop. An endless loop faults (see Errors and faults); it can\'t hang ' +
      'the board.',
    example: '(defn dim (c:color k:decimal) (lerp #000000 c k))\n(defn wrap (x:int n:int) (if (ge x n) (wrap (- x n) n) x))   ; a loop\n' +
      '(fill (buf O) (dim #FF8000 0.25))\n(set (buf O) (wrap (env frame) 50) #FFFFFF)',
  },
  {
    name: 'fn', category: 'Functions', summary: 'A function without a name, written where a function is passed (map-px, repeat).',
    args: [
      { name: 'params', type: 'parameters', desc: '(name:type …); what the receiving function passes in' },
      { name: 'body', type: 'body', desc: 'one or more forms; the last one\'s value is the result', many: 2 },
    ],
    returns: 'a function (only as an argument; it can\'t be stored in a variable)',
    details: 'Unlike defn, a fn sees the variables around it: the program\'s variables, or the parameters and locals of ' +
      'the function it is written in. A def inside it assigns such a variable, or else makes a local.',
    example: '(def k 0.5)\n(fill (buf O) #FF8000)\n(map-px (buf O) (buf O) (fn (c:color) (lerp #000000 c k)))',
  },
  {
    name: 'use', category: 'Functions', summary: 'Make the functions of a library program available in this program.',
    args: [{ name: 'name', type: 'name', desc: 'a program in the library' }],
    returns: 'nothing',
    details: 'Only at the top level, anywhere in the program. The library program may contain only defn and use. Its ' +
      'functions, and those of the libraries it uses, can then be called like the program\'s own; only the ones actually ' +
      'reached are compiled in, and they count toward the program\'s limits.\n\nThe functions are copied into the program ' +
      'when it compiles. On a board, saving or deleting a library program first compiles the running programs that use ' +
      'libraries against the change: if one would break, the change is refused and the error says which. Otherwise they ' +
      'switch to the new functions.\n\nIn the IDE, use reads the connected board\'s library. With no board connected it ' +
      'reads the local library by name (a name in two folders is an error). Uploading sends the libraries a selection ' +
      'uses first. Errors inside a library are shown at its use, with the library\'s own line number.',
    example: '; needs a library program named shades, containing:\n;   (defn dim (c:color k:decimal) (lerp #000000 c k))\n(use shades)\n(fill (buf O) (dim #FF8000 0.25))',
  },
  {
    name: 'map-px', category: 'Functions', summary: 'Compute every pixel of dst with a function of the matching src pixel (and its index).',
    args: [
      ...dstSrc('write'),
      { name: 'f', type: 'function', desc: 'a defn name or (fn …) taking (c:color) or (c:color i:int), returning a color' },
      ...range,
    ],
    returns: 'dst',
    details: 'For each pixel i of the range, dst[i] becomes (f src[i] i), over the shorter of the two views. i counts from ' +
      'the views\' start. dst and src may be the same buffer (in place); views that overlap otherwise are read as if src were ' +
      'copied first.\n\nEvery pixel is a function call: a long body on a long string costs time, and on the board loops ' +
      'must fit in about 12 ms per frame. Watch the render time in `stat`.',
    example: '(defn wave (c:color i:int) (lerp #000040 #00C0FF (tri (+ (* i 12) (* (env frame) 4)))))\n(map-px (buf O) (buf O) wave)',
  },
  {
    name: 'repeat', category: 'Functions', summary: 'Call a function n times, with i = 0, 1, … n-1.',
    args: [
      { name: 'n', type: 'int', desc: 'how many times; 0 or less does nothing' },
      { name: 'f', type: 'function', desc: 'a defn name or (fn …) taking (i:int) or nothing' },
    ],
    returns: 'nothing',
    details: 'Use it to draw several things, or to add up a value: a def inside the fn assigns a variable outside it.',
    example: '(fill (buf O) #000000)\n(repeat 5 (fn (i:int) (set (buf O) (* i 10) #FF0000)))\n' +
      '(def n 0)\n(repeat 4 (fn (i:int) (def n (+ n i))))   ; n = 0+1+2+3\n(set (buf O) n #00FF00)',
  },
];

/* ---------------- topic pages ---------------- */

export interface Topic { id: string; title: string; body: string; example?: string }

export const TOPICS: Topic[] = [
  {
    id: 'programs', title: 'How programs run',
    body: 'A program is a list of calls, run top to bottom 60 times a second, once per frame. Each channel (each LED ' +
      'string) runs its own program.\n\nAfter every frame, buffer O is sent to the LEDs, scaled by the global intensity. ' +
      'The buffers X, Y, Z and O keep their contents between frames, so a program can either redraw everything each frame ' +
      'or keep changing what is already there.\n\nComments start with ; and run to the end of the line. Programs sent from ' +
      'the IDE may be up to 3999 characters; a console line holds 511.',
    example: '; nothing is cleared: each frame fades what is there a little and adds a white pixel, leaving trails\n(scale (buf O) 0.92)\n(set (buf O) (mod (* (env frame) 7) 50) #FFFFFF)',
  },
  {
    id: 'values', title: 'Values and types',
    body: 'Every argument has a kind, checked when the program compiles:\n\n• buffer: (buf O), a slice, or a variable holding one\n' +
      '• color: #RRGGBB or #RRGGBBWW (W is ignored on RGB strings)\n• int: whole numbers like 3 or -10\n• number: an int or a ' +
      'decimal like 0.5 (decimals have about 5 digits of precision and range ±32767)\n• 0..1: a number used as an amount; ' +
      'clamped to 0..1, with 1/256 steps\n• boolean: t or nil\n• string: text in double quotes, "like this" (at most 255 ' +
      'characters on one line; write \\" for a quote and \\\\ for a backslash). Strings are for displays (seg7, seg9, ' +
      'seg14) and the Text functions.\n\nDecimals never turn into ints by themselves: use floor or ' +
      'round where an index is needed.',
  },
  {
    id: 'ranges', title: 'Ranges and positions',
    body: 'Most functions take an optional [start] [end] range, always with the same rules: absolute pixel indices, end ' +
      'exclusive, default the whole buffer, negative indices count from the end (-1 is the last pixel), and values outside ' +
      'the buffer are clamped. An empty range does nothing.\n\nSingle positions (set and get) also count negatives from the ' +
      'end, but positions outside the buffer are skipped (get returns black), so a moving dot disappears off the edge ' +
      'instead of sticking to it.',
    example: '(fill (buf O) #000000)\n(fill (buf O) #FF0000 0 5)     ; pixels 0..4\n(fill (buf O) #0000FF -5)      ; the last 5\n(fill (buf O) #00FF00 20 -20)  ; the middle',
  },
  {
    id: 'defaults', title: 'Optional arguments and _',
    body: 'Optional arguments can be left out from the end. To skip one in the middle, write _ for it: it means "the ' +
      'default". (slice (buf O) _ _ -1) is O reversed; (fill-pat (buf O) pal _ _ _ 10 20) fills only 10..19.',
  },
  {
    id: 'logic', title: 'Booleans and conditions',
    body: 'Booleans are their own kind: t and nil. Conditions (if, and, or, not) take only booleans; a number is never true ' +
      'or false by itself, so compare it: (if (gt x 0) …). Comparisons (lt gt le ge eq ne) return booleans.',
  },
  {
    id: 'faults', title: 'Errors and faults',
    body: 'Compile errors (shown while you type, with line and column) stop a program from being loaded: the previous ' +
      'program keeps running.\n\nSome problems can only happen while running, such as dividing by a value that turns out ' +
      'to be 0. They are faults: the rest of that frame\'s program is skipped, the fault is counted (see the preview\'s stats ' +
      'or `stat` on the board), and the next frame tries again. A program can never crash the board or write outside a ' +
      'buffer. A frame may evaluate at most 65536 calls (loops included). On the board, function calls and loops must also ' +
      'finish within about 12 ms of each frame; past that they fault, which keeps a runaway loop from starving the board. Calls ' +
      'nest at most 12 deep in the source, and the stack they need, counting the functions they call, is checked when ' +
      'compiling.',
  },
];

/** Argument counts implied by an entry, to compare with the compiler's table. */
export function argCounts(e: Entry): { min: number; max: number } {
  return {
    min: e.args.filter((a) => !a.optional).length,
    max: e.args.reduce((n, a) => n + (a.many ?? 1), 0),
  };
}

/** (fill <buf> <color> [start] [end]) */
export function signature(e: Entry): string {
  const parts = e.args.map((a) => {
    const s = a.optional ? `[${a.name}]` : `<${a.name}>`;
    return a.many && a.many > 1 ? `${s}…` : s;
  });
  return `(${[e.name, ...parts].join(' ')})`;
}

export const ENTRY_BY_NAME = new Map(ENTRIES.map((e) => [e.name, e]));
