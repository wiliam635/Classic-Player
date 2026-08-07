/*
 * Detector harmônico local inspirado no modelo de busca por rotações usado por
 * @tonaljs/chord-detect (MIT): https://github.com/tonaljs/tonal
 *
 * Esta adaptação trabalha diretamente com números MIDI, usa o baixo real como
 * critério de inversão e permanece disponível quando o Classic Player está
 * totalmente offline.
 */

const pitchClass = note => ((note % 12) + 12) % 12;

export function detectChordCandidates(noteValues, patterns, { assumePerfectFifth = true } = {}) {
  const notes = [...noteValues].filter(Number.isFinite).sort((a, b) => a - b);
  const orderedPitchClasses = [];
  for (const midiNote of notes) {
    const pc = pitchClass(midiNote);
    if (!orderedPitchClasses.includes(pc)) orderedPitchClasses.push(pc);
  }

  if (!orderedPitchClasses.length) return { bass: null, pitchClasses: [], candidates: [] };

  const bass = orderedPitchClasses[0];
  const pitchClasses = [...orderedPitchClasses].sort((a, b) => a - b);
  const candidates = [];

  for (const root of pitchClasses) {
    const intervals = pitchClasses.map(pc => (pc - root + 12) % 12).sort((a, b) => a - b);

    patterns.forEach(([pattern, suffix], index) => {
      const exact = pattern.length === intervals.length && pattern.every((value, i) => value === intervals[i]);
      let impliedFifth = false;

      if (!exact && assumePerfectFifth && !intervals.includes(7)) {
        const completed = [...intervals, 7].sort((a, b) => a - b);
        const hasThird = pattern.includes(3) || pattern.includes(4);
        const hasSeventh = pattern.includes(10) || pattern.includes(11);
        const hasAlteredFifth = pattern.includes(6) || pattern.includes(8);
        impliedFifth = hasThird && hasSeventh && !hasAlteredFifth &&
          pattern.length === completed.length && pattern.every((value, i) => value === completed[i]);
      }

      if (exact || impliedFifth) {
        candidates.push({
          root,
          suffix,
          impliedFifth,
          score: (root === bass ? 1000 : 500) + patterns.length - index - (impliedFifth ? 120 : 0),
        });
      }
    });
  }

  candidates.sort((a, b) => b.score - a.score);
  return { bass, pitchClasses, candidates };
}
