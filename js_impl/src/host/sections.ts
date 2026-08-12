// sections.ts — .pss section splitter (port of c_impl/src/eval/pd_section.c).
export const SEC_HOST = 3, SEC_VERTEX = 0, SEC_GEOMETRY = 1, SEC_FRAGMENT = 2;

export interface Section { type: number; start: number; end: number; line: number; name: string; }
export interface SectionList { secs: Section[]; err: string; }

export function sectionParse(src: string): SectionList {
  const sl: SectionList = { secs: [], err: '' };
  let i = 0, line = 1;
  let curStart = 0, curLine = 1, curType = SEC_HOST;
  let pendingName = '';

  const pushBlock = (end: number) => {
    if (sl.secs.length >= 64) { sl.err = 'too many sections'; return; }
    let e = end;
    while (e > curStart && (src[e - 1] === '\n' || src[e - 1] === '\r')) e--;
    if (e > curStart || sl.secs.length === 0) {
      sl.secs.push({ type: curType, start: curStart, end: e, line: curLine, name: pendingName });
    }
    pendingName = '';
  };

  while (i < src.length) {
    const atLineStart = (i === 0) || (src[i - 1] === '\n');
    if (atLineStart) {
      let j = i;
      while (src[j] === ' ' || src[j] === '\t') j++;
      if (src[j] === '@') {
        let blockEnd = i;
        while (blockEnd > curStart && (src[blockEnd - 1] === '\n' || src[blockEnd - 1] === '\r')) blockEnd--;
        if (blockEnd > curStart) pushBlock(blockEnd);
        const m = src[j + 1];
        let nt = curType;
        if (m === 'h' || m === 'H') nt = SEC_HOST;
        else if (m === 'v' || m === 'V') nt = SEC_VERTEX;
        else if (m === 'g' || m === 'G') nt = SEC_GEOMETRY;
        else if (m === 'f' || m === 'F') nt = SEC_FRAGMENT;
        curType = nt;
        curLine = line;
        pendingName = '';
        let k = j + 1;
        while (k < src.length && src[k] !== '\n' && src[k] !== ':') k++;
        if (src[k] === ':') {
          k++;
          let ni = 0;
          while (k < src.length && src[k] !== '\n' && src[k] !== ' ' && src[k] !== '\t' && ni < 31) {
            pendingName += src[k]; k++; ni++;
          }
        }
        while (k < src.length && src[k] !== '\n') k++;
        curStart = k; i = k; continue;
      }
    }
    if (src[i] === '\n') line++;
    i++;
  }
  let blockEnd = i;
  while (blockEnd > curStart && (src[blockEnd - 1] === '\n' || src[blockEnd - 1] === '\r')) blockEnd--;
  if (blockEnd > curStart) pushBlock(blockEnd);
  else if (sl.secs.length === 0) pushBlock(0);
  return sl;
}

export function sectionHost(sl: SectionList): Section | null {
  let last: Section | null = null;
  for (const s of sl.secs) if (s.type === SEC_HOST) last = s;
  return last;
}
export function sectionBlocks(src: string, sl: SectionList): { src: string; name: string; type: number }[] {
  return sl.secs
    .filter((s) => s.type !== SEC_HOST)
    .map((s) => ({ src: src.slice(s.start, s.end), name: s.name, type: s.type }));
}
