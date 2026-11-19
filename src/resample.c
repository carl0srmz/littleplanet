#include "resample.h"
#include <math.h>

#define RS_RES 256
static const double PI = 3.14159265358979323846;

static double bessel_i0(double x) {
  double s = 1, t = 1;
  for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
  return s;
}

void rs_init(Resampler *r, double in_rate, double out_rate) {
  memset(r, 0, sizeof *r);
  r->step = in_rate / out_rate;
  double ratio = out_rate < in_rate ? out_rate / in_rate : 1.0;
  double cutoff = 0.5 * ratio * 0.92;            // cycles per input sample, a bit under Nyquist
  r->half = (int)ceil(16.0 / ratio);
  int n = r->half * RS_RES + 2;
  r->table = malloc(sizeof(float) * n);
  const double beta = 8.6, norm = bessel_i0(beta);
  double sum = 0;
  for (int i = 0; i < n; i++) {
    double x = (double)i / RS_RES;
    double s = x == 0 ? 2 * cutoff : sin(2 * PI * cutoff * x) / (PI * x);
    double w = x >= r->half ? 0 : bessel_i0(beta * sqrt(1 - (x / r->half) * (x / r->half))) / norm;
    r->table[i] = (float)(s * w);
    if (i % RS_RES == 0) sum += (i ? 2 : 1) * r->table[i];
  }
  for (int i = 0; i < n; i++) r->table[i] /= (float)sum;   // unity DC gain
  r->pos = r->half;   // start once a full window is available
}

void rs_free(Resampler *r) { free(r->table); r->table = NULL; }

// The taps are 1 input sample apart, so on each side of the output position they all sit at the same
// fraction between two table points: |pos - n| * RS_RES is that side's first offset plus RS_RES per tap
// (all exact in double), so the interpolation weight and the table index step are worked out once a side.
// Same taps, same order, same arithmetic per tap as the straightforward loop: bit-identical output.
void rs_pop(Resampler *r, float *l, float *rr) {
  double p = r->pos;
  long base = (long)floor(p);
  double frac = p - (double)base;
  const float *t = r->table, *ringl = r->ring[0], *ringr = r->ring[1];
  const int half = r->half;
  float al = 0, ar = 0;
  {   // n = base - half + 1 .. base: distance frac + k, k = half - 1 .. 0
    double x0 = frac * RS_RES;
    int i0 = (int)x0;
    float f = (float)(x0 - i0);
    long n = base - half + 1;
    for (int k = half - 1; k >= 0; k--, n++) {
      int i = i0 + k * RS_RES;
      float w = t[i] + (t[i + 1] - t[i]) * f;
      al += w * ringl[n & (RS_RING - 1)];
      ar += w * ringr[n & (RS_RING - 1)];
    }
  }
  {   // n = base + 1 .. base + half: distance (1 - frac) + k, k = 0 .. half - 1
    double x0 = (1.0 - frac) * RS_RES;
    int i0 = (int)x0;
    float f = (float)(x0 - i0);
    long n = base + 1;
    for (int k = 0; k < half; k++, n++) {
      int i = i0 + k * RS_RES;
      float w = t[i] + (t[i + 1] - t[i]) * f;
      al += w * ringl[n & (RS_RING - 1)];
      ar += w * ringr[n & (RS_RING - 1)];
    }
  }
  *l = al; *rr = ar;
  r->pos += r->step;
}
