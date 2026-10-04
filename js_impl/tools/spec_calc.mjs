// Precise re-computation of drawcone2 fragment shader at a pixel, from the
// JS debug capture (v + uniforms). Mirrors the GLSL statements exactly.
const uni = {
  ligdir: [-0.5773502691896258, 0.5773502691896258, 0.5773502691896258],
  p0: [-0.32952698492462335, 2.122417438109626, 11.467288341052772],
  Zc0: -136.0719457122302,
  p1: [-0.2344830196162627, 2.372687643657203, 11.773011514408061],
  Zc1: -144.1984290592367,
  np0: [-0.34103803419196443, 2.0921064903618225, 11.43026132414283],
  np1: [-0.25174959351727433, 2.327221222035498, 11.717470989043148],
  dnm: [0.23388627228078157, 0.6158704053282879, 0.7523302835054367],
  nms: [-0.057555246336705515, -0.1515547387390163, -0.18513508454970964],
  nnm: [0.0892884406746901, 0.2351147316736757, 0.28720966490031863],
  k0: 6.861498726291737,
  k1: 3.744315826460548,
  kxw: [-0.14610183451125497, 0.04757334546756043, 0.05811428536468416],
  kyw: [-0.09250001406796532, 0.1530268088725718],
  kzz: -0.06166853112636336,
  kw: [-0.42993550022259286, -0.668233838683074, 0.5604670266234777],
  k: -5.223949271065942,
  r0: 0.2,
  dr: 0.09999999999999998,
  rr: [5, 3.3333333333333335],
  fnm: [-0.23388627228078157, -0.6158704053282879, -0.7523302835054367],
  fp: [-0.34103803419196443, 2.0921064903618225, 11.43026132414283],
  fr2: 0,
  zscale: [-0.01000010000100001, 1.000010000100001],
};
const v = [0.2640770704934247, -2.0598011498487097, -11.26728834105277, 1];
const c = [0.5603852218662242, 0.6304333745995023, 0.7004815273327802, 1];
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const mul3 = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const neg3 = (a) => [-a[0], -a[1], -a[2]];
const norm3 = (a) => { const l = Math.hypot(a[0], a[1], a[2]); return mul3(a, 1 / l); };
const V = v.slice(0, 3);
const D = neg3(V); // -v.xyz

let Za, Zb, insqr, f, d, fmin, norm;
Za = v[0] * dot3(V, uni.kxw) + v[1] * (v[1] * uni.kyw[0] + v[2] * uni.kyw[1]) + v[2] * v[2] * uni.kzz;
Zb = dot3(D, uni.kw);
insqr = Zb * Zb - Za * uni.k;
f = (Math.sqrt(insqr) - Zb) / Za;
d = (dot3(D, uni.nnm) * f - uni.k1) * uni.k0;
console.log('cone: Za=%s Zb=%s insqr=%s f=%s d=%s', Za, Zb, insqr, f, d);
if (insqr >= 0.0 && d > 0.0 && d < 1.0) {
  fmin = f;
  const hit = [V[0] * f + uni.nnm[0] * d + uni.np0[0], V[1] * f + uni.nnm[1] * d + uni.np0[1], V[2] * f + uni.nnm[2] * d + uni.np0[2]];
  const sc = uni.dr * d + uni.r0;
  norm = sub3(uni.nms, mul3(hit, 1 / sc));
  console.log('CONE branch');
} else {
  fmin = 1e32;
  Za = dot3(D, D); d = 1.0 / Za;
  Zb = dot3(D, uni.p0); insqr = Zb * Zb - Za * uni.Zc0;
  f = (Math.sqrt(insqr) - Zb) * d;
  let nhit = mul3(D, f);
  if (insqr >= 0.0 && f < fmin && dot3(sub3(nhit, uni.np1), uni.dnm) <= 0.0) {
    fmin = f; norm = mul3(sub3(nhit, uni.p0), uni.rr[0]);
    console.log('SPHERE0 branch f=%s |nhit-p0|=%s (r0=0.2)', f, Math.hypot(...sub3(nhit, uni.p0)));
  }
  Zb = dot3(D, uni.p1); insqr = Zb * Zb - Za * uni.Zc1;
  f = (Math.sqrt(insqr) - Zb) * d;
  nhit = mul3(D, f);
  if (insqr >= 0.0 && f < fmin && dot3(sub3(nhit, uni.np0), uni.dnm) >= 0.0) {
    fmin = f; norm = mul3(sub3(nhit, uni.p1), uni.rr[1]);
    console.log('SPHERE1 branch f=%s |nhit-p1|=%s (r1=0.3)', f, Math.hypot(...sub3(nhit, uni.p1)));
  }
  f = dot3(neg3(uni.fp), uni.fnm) / dot3(V, uni.fnm);
  const tp = sub3(mul3(D, f), uni.fp);
  if (dot3(tp, tp) < uni.fr2) { fmin = f; norm = uni.fnm; console.log('PLANE branch'); }
  if (fmin >= 1e32) { console.log('DISCARD'); process.exit(0); }
}
console.log('norm=', norm, '|n|=', Math.hypot(...norm));
let fl = dot3(norm, uni.ligdir) * -0.5 + 0.5;
const u = norm3(D);
const ndl = dot3(norm, uni.ligdir);
const r = sub3(mul3(norm, ndl * 2.0), uni.ligdir);
let dd = Math.max(dot3(r, u), 0.0);
dd *= dd; dd *= dd; dd *= dd;
fl += dd;
console.log('u=%s dot(r,u)=%s d^8=%s f_final=%s', u, dot3(r, u), dd, fl);
console.log('gl_FragColor=', [c[0] * fl, c[1] * fl, c[2] * fl]);
