const RATE = 48000;
// ---- program signal (8 s loop): ambient / speech / music+kick / explosion
function rng(seed) { let s = seed >>> 0; return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296) * 2 - 1; }
function program(sec) {
  const n = sec * RATE, L = new Float32Array(n), R = new Float32Array(n), r = rng(1);
  let b0 = 0, b1 = 0, lp = 0;
  const db = (d) => Math.pow(10, d / 20);
  for (let i = 0; i < n; i++) {
    const t = (i / RATE) % 8, w = r();
    b0 = 0.99 * b0 + 0.1 * w; b1 = 0.9 * b1 + 0.3 * w; const pink = (b0 + b1 + 0.2 * w) * 0.5;
    lp = lp + 0.15 * (w - lp);
    let x = 0;
    if (t < 2) x = pink * db(-36) + Math.sin(2 * Math.PI * 220 * i / RATE) * db(-30);
    else if (t < 4) { const syl = Math.max(0, Math.sin(2 * Math.PI * 4 * t)) ** 2; x = (lp * 2 * db(-12)) * syl + Math.sin(2 * Math.PI * 180 * i / RATE) * syl * db(-18); }
    else if (t < 6) { const tn = (t - 4) % 0.5, env = Math.exp(-tn * 5), k = Math.exp(-tn * 18);
      for (const f of [220, 277.2, 329.6, 440]) x += Math.sin(2 * Math.PI * f * i / RATE) * env * db(-16);
      x += Math.sin(2 * Math.PI * 55 * tn * (1 + 2 * Math.exp(-tn * 30))) * k * db(-6); }
    else { const tt = t - 6, env = tt < 0.01 ? tt / 0.01 : Math.exp(-(tt - 0.01) * 2.5); x = (w * 0.5 + lp * 1.5) * env * db(-7); }
    L[i] = x; R[i] = x * 0.9 + pink * db(-40);
  }
  return [L, R];
}
// ---- current limiter (gain.c port)
const KNEE = 0.8;
function kneeGain(a) { if (a <= KNEE) return 1; const r = 1 - KNEE, u = (a - KNEE) / r; return (KNEE + r * (u / (1 + u))) / a; }
function limiter(L, R, rel_s = 0.1, hold = 480) {
  const REL = 1 - 1 / (rel_s * RATE); let env = 1, h = 0;
  for (let i = 0; i < L.length; i++) {
    const a = Math.max(Math.abs(L[i]), Math.abs(R[i])), want = kneeGain(a);
    if (want < env) { env = want; h = hold; } else if (want < 1 && want <= env * 1.02) h = hold; else if (h > 0) h--; else { env = 1 - (1 - env) * REL; if (want < env) env = want; }
    L[i] *= env; R[i] *= env;
  }
}
// ---- candidate boost stage
function boost(L, R, g, P) {
  const a = Math.min(1, Math.max(0, (g - 1) / 4));
  if (a <= 0) return;
  // 1) leveling compressor (stereo-linked, smoothed peak detector), dB domain
  const thr = P.thr, ratio = 1 + P.ratio * a, slope = 1 - 1 / ratio, knee = P.knee;
  const att = Math.exp(-1 / (P.att * RATE)), rel = Math.exp(-1 / (P.rel * RATE));
  const makeup = P.auto ? a * slope * (-thr) * P.auto : P.makeup * a;
  let env = 0, gg = 1;
  for (let i = 0; i < L.length; i++) {
    const x = Math.max(Math.abs(L[i]), Math.abs(R[i]));
    env = x > env ? att * env + (1 - att) * x : rel * env + (1 - rel) * x;
    if ((i & 7) === 0) {
      const lv = 20 * Math.log10(env + 1e-9), over = lv - thr;
      const gr = 2 * over < -knee ? 0 : 2 * Math.abs(over) <= knee ? slope * (over + knee / 2) ** 2 / (2 * knee) : slope * over;
      gg = Math.pow(10, (makeup - gr) / 20);
    }
    L[i] *= gg; R[i] *= gg;
  }
  // 2) gentle soft saturation of the tops only (odd, smooth), amount by a
  if (P.sat) {
    const k = 1 - P.sat * a;            // linear below k
    for (const C of [L, R]) for (let i = 0; i < C.length; i++) {
      const x = C[i], ax = Math.abs(x);
      if (ax > k) { const u = (ax - k) / (1 - k); C[i] = Math.sign(x) * (k + (1 - k) * Math.tanh(u) * 1.0); }
    }
  }
}
function chain(src, g, P) {
  const L = Float32Array.from(src[0], (v) => v * g), R = Float32Array.from(src[1], (v) => v * g);
  if (P) boost(L, R, g, P);
  limiter(L, R, P && P.limRel ? 0.1 - (0.1 - P.limRel) * Math.min(1, (g - 1) / 4) : 0.1);
  return [L, R];
}
function rms(C) { let s = 0; for (const v of C[0]) s += v * v; for (const v of C[1]) s += v * v; return 10 * Math.log10(s / (C[0].length * 2)); }
function peak(C) { let p = 0; for (const v of C[0]) p = Math.max(p, Math.abs(v)); return 20 * Math.log10(p); }
// THD-ish: sine 100 Hz at -6 dBFS, measure residual after removing fundamental (least squares)
function thd(g, P, f = 100) {
  const n = RATE, s = new Float32Array(n); for (let i = 0; i < n; i++) s[i] = 0.5 * Math.sin(2 * Math.PI * f * i / RATE);
  const o = chain([s, s.slice()], g, P)[0].slice(RATE / 2);
  let c = 0, d = 0, cc = 0, dd = 0, cd = 0, oc = 0, od = 0;
  for (let i = 0; i < o.length; i++) { const t = (i + RATE / 2) / RATE, a = Math.sin(2 * Math.PI * f * t), b = Math.cos(2 * Math.PI * f * t); cc += a * a; dd += b * b; oc += o[i] * a; od += o[i] * b; }
  const A = oc / cc, B = od / dd; let res = 0, tot = 0;
  for (let i = 0; i < o.length; i++) { const t = (i + RATE / 2) / RATE, fit = A * Math.sin(2 * Math.PI * f * t) + B * Math.cos(2 * Math.PI * f * t); res += (o[i] - fit) ** 2; tot += fit ** 2; }
  return (100 * Math.sqrt(res / tot)).toFixed(2) + '%';
}
const fs = require('fs'); const wb = fs.readFileSync(process.argv[2]); const pc = new Int16Array(wb.buffer.slice(wb.byteOffset + 44)); const src = [new Float32Array(pc.length / 2), new Float32Array(pc.length / 2)]; for (let i = 0; i < pc.length / 2; i++) { src[0][i] = pc[2 * i] / 32768; src[1][i] = pc[2 * i + 1] / 32768; }
console.log('source RMS', rms(src).toFixed(2), 'peak', peak(src).toFixed(2));
function thdAt(g, P, f, amp) { const n = RATE, s = new Float32Array(n); for (let i = 0; i < n; i++) s[i] = amp * Math.sin(2 * Math.PI * f * i / RATE);
  const o = chain([s, s.slice()], g, P)[0].slice(RATE / 2); let cc = 0, dd = 0, oc = 0, od = 0;
  for (let i = 0; i < o.length; i++) { const t = (i + RATE / 2) / RATE, a = Math.sin(2 * Math.PI * f * t), b = Math.cos(2 * Math.PI * f * t); cc += a * a; dd += b * b; oc += o[i] * a; od += o[i] * b; }
  const A = oc / cc, B = od / dd; let res = 0, tot = 0;
  for (let i = 0; i < o.length; i++) { const t = (i + RATE / 2) / RATE, fit = A * Math.sin(2 * Math.PI * f * t) + B * Math.cos(2 * Math.PI * f * t); res += (o[i] - fit) ** 2; tot += fit ** 2; }
  return (100 * Math.sqrt(res / tot)).toFixed(1) + '%'; }
const cands = {
  old: null,
  J: { thr: -12, ratio: 3, knee: 6, att: 0.002, rel: 0.08, auto: 0.8, sat: 0, limRel: 0.06 },
  M: { thr: -12, ratio: 3, knee: 6, att: 0.002, rel: 0.08, auto: 1.2, sat: 0, limRel: 0.06 },
  N: { thr: -12, ratio: 3, knee: 6, att: 0.002, rel: 0.08, auto: 1.5, sat: 0, limRel: 0.06 },
  O: { thr: -10, ratio: 3, knee: 6, att: 0.002, rel: 0.08, auto: 1.5, sat: 0, limRel: 0.06 },
};
for (const [name, P] of Object.entries(cands)) {
  const row = [200, 300, 400, 500].map((pct) => { const o = chain(src, pct / 100, P); return pct + ':' + rms(o).toFixed(1); });
  console.log(name.padEnd(4), row.join(' '), '| THD 100Hz/1k -12dBFS @300', thdAt(3, P, 100, 0.25), thdAt(3, P, 1000, 0.25), '@500', thdAt(5, P, 100, 0.25), thdAt(5, P, 1000, 0.25));
}
