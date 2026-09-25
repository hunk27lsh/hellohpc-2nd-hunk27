"""Kernel-field solver: runtime-compiled OpenMP C kernel with NumPy fallback."""
from __future__ import annotations

import ctypes
import os
import platform
import subprocess
import tempfile

import numpy as np

_C_SOURCE = r"""
#include <stdint.h>
#include <omp.h>

/* exp(x): double-precision Cody-Waite reduction + degree-6 Taylor (rel err ~1e-7) */
static inline float kf_exp(float x) {
    const double LOG2E = 1.4426950408889634, LN2 = 0.6931471805599453;
    double xd = (double)x;
    double nd = (double)(int)(xd * LOG2E + (xd >= 0.0 ? 0.5 : -0.5));
    float r = (float)(xd - nd * LN2);
    float p = 1.0f + r * (1.0f
        + r * (0.5f
        + r * (1.0f / 6.0f
        + r * (1.0f / 24.0f
        + r * (1.0f / 120.0f
        + r * (1.0f / 720.0f))))));
    int e = (int)nd + 127;
    e = e < 0 ? 0 : (e > 254 ? 254 : e);
    uint32_t u = (uint32_t)e << 23;
    float sc;
    __builtin_memcpy(&sc, &u, 4);
    return p * sc;
}

/* sin(x): double-precision reduction, degree-9/10 Taylor (abs err ~5e-7 for |x|<=20) */
static inline float kf_sin(float x) {
    const double TWO_OVER_PI = 0.6366197723675814, PIO2 = 1.5707963267948966;
    double xd = (double)x;
    double kd = (double)(int)(xd * TWO_OVER_PI + (xd >= 0.0 ? 0.5 : -0.5));
    float r = (float)(xd - kd * PIO2);
    float r2 = r * r;
    float sp = r * (1.0f + r2 * (-0.16666667f
        + r2 * (8.3333333e-3f
        + r2 * (-1.9841270e-4f
        + r2 * (2.7557319e-6f)))));
    float cp = 1.0f + r2 * (-0.5f
        + r2 * (4.1666667e-2f
        + r2 * (-1.3888889e-3f
        + r2 * (2.4801587e-5f))));
    int k = (int)kd;
    float sel = (float)(k & 1);
    float sign = 1.0f - 2.0f * (float)((k >> 1) & 1);
    return sign * ((1.0f - sel) * sp + sel * cp);
}

#define KF_R 4
#define KF_TILE 256

void field_kernel(const float *x, long long Q,
                  const float *mu, const float *vv,
                  const float *w, const float *s,
                  const float *b, const float *t,
                  long long C, int D,
                  float *mu_t, float *v_t, float *out)
{
    #pragma omp parallel for schedule(static)
    for (int d = 0; d < D; d++)
        for (long long c = 0; c < C; c++) {
            mu_t[(long long)d * C + c] = mu[c * D + d];
            v_t[(long long)d * C + c] = vv[c * D + d];
        }

    #pragma omp parallel for schedule(static)
    for (long long q0 = 0; q0 < Q; q0 += KF_R) {
        int rn = (Q - q0 < KF_R) ? (int)(Q - q0) : KF_R;
        double r2[KF_R][KF_TILE], pv[KF_R][KF_TILE], acc[KF_R];
        for (int r = 0; r < rn; r++) acc[r] = 0.0;
        for (long long c0 = 0; c0 < C; c0 += KF_TILE) {
            long long cn = C - c0;
            if (cn > KF_TILE) cn = KF_TILE;
            for (int r = 0; r < rn; r++)
                for (long long c = 0; c < cn; c++) { r2[r][c] = 0.0; pv[r][c] = 0.0; }
            for (int d = 0; d < D; d++) {
                const float *mr = mu_t + (long long)d * C + c0;
                const float *vr = v_t + (long long)d * C + c0;
                for (int r = 0; r < rn; r++) {
                    float xd = x[(q0 + r) * D + d];
                    #pragma omp simd
                    for (long long c = 0; c < cn; c++) {
                        double df = (double)xd - (double)mr[c];
                        r2[r][c] += df * df;
                        pv[r][c] += (double)xd * (double)vr[c];
                    }
                }
            }
            const float *wr = w + c0, *sr = s + c0, *br = b + c0, *tr = t + c0;
            for (int r = 0; r < rn; r++) {
                double tacc = 0.0;
                #pragma omp simd reduction(+:tacc)
                for (long long c = 0; c < cn; c++) {
                    float g = wr[c] * kf_exp(-sr[c] * (float)r2[r][c]);
                    float h = br[c] * kf_sin(tr[c] * (float)pv[r][c]);
                    tacc += (double)(g + h);
                }
                acc[r] += tacc;
            }
        }
        for (int r = 0; r < rn; r++) out[q0 + r] = (float)acc[r];
    }
}
"""


def _build():
    workdir = tempfile.mkdtemp(prefix="kf-kernel-")
    cpath = os.path.join(workdir, "kernel.c")
    sopath = os.path.join(workdir, "kernel.so")
    with open(cpath, "w", encoding="utf-8") as stream:
        stream.write(_C_SOURCE)
    machine = platform.machine().lower()
    native = ["-mcpu=native"] if machine in ("aarch64", "arm64") else ["-march=native"]
    flags = ["-O3", "-fno-math-errno", "-funroll-loops", "-fopenmp",
             "-shared", "-fPIC", cpath, "-o", sopath, "-lm"]
    for extra in (native, []):
        try:
            result = subprocess.run(["cc", *extra, *flags], capture_output=True,
                                    timeout=240)
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode or not os.path.exists(sopath):
            continue
        try:
            lib = ctypes.CDLL(sopath)
        except OSError:
            continue
        lib.field_kernel.restype = None
        lib.field_kernel.argtypes = [
            ctypes.c_void_p, ctypes.c_longlong,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_longlong, ctypes.c_int,
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        return lib
    return None


_LIB = _build()


def _run_c(points, centers, trig_vec, weights, scales, bias, trig_scale,
           q_count, c_count, dim):
    mu_t = np.empty((dim, c_count), dtype=np.float32)
    v_t = np.empty((dim, c_count), dtype=np.float32)
    out = np.empty(q_count, dtype=np.float32)
    _LIB.field_kernel(points.ctypes.data, q_count,
                      centers.ctypes.data, trig_vec.ctypes.data,
                      weights.ctypes.data, scales.ctypes.data,
                      bias.ctypes.data, trig_scale.ctypes.data,
                      c_count, dim,
                      mu_t.ctypes.data, v_t.ctypes.data, out.ctypes.data)
    return out


def _compute_numpy(points, centers, weights, scales, bias, trig_scale, trig_vec,
                   q_count):
    out = np.empty(q_count, dtype=np.float32)
    mu2 = np.einsum("cd,cd->c", centers, centers)
    for i in range(0, q_count, 4096):
        x = points[i:i + 4096]
        x2 = np.einsum("qd,qd->q", x, x)
        r2 = x @ centers.T
        r2 *= np.float32(-2.0)
        r2 += x2[:, None]
        r2 += mu2
        p = x @ trig_vec.T
        r2 *= scales
        np.negative(r2, out=r2)
        np.exp(r2, out=r2)
        r2 *= weights
        p *= trig_scale
        np.sin(p, out=p)
        p *= bias
        r2 += p
        np.sum(r2, axis=1, out=out[i:i + 4096])
    return out


if _LIB is not None:
    rng = np.random.default_rng(0)
    qn, cn, dn = 96, 192, 16
    _pts = rng.standard_normal((qn, dn), dtype=np.float32) * 3
    _cen = rng.standard_normal((cn, dn), dtype=np.float32) * 2
    _w = rng.random(cn, dtype=np.float32) - 0.5
    _s = rng.random(cn, dtype=np.float32) * 0.4 + 0.01
    _b = rng.random(cn, dtype=np.float32) - 0.5
    _t = rng.random(cn, dtype=np.float32) * 1.5 + 0.1
    _v = rng.standard_normal((cn, dn), dtype=np.float32)
    _got = _run_c(_pts, _cen, _v, _w, _s, _b, _t, qn, cn, dn)
    _ref = _compute_numpy(_pts, _cen, _w, _s, _b, _t, _v, qn)
    if not np.allclose(_got, _ref, rtol=1e-4, atol=1e-5):
        _LIB = None


def compute_field(points, centers, weights, scales, bias, trig_scale, trig_vec):
    q_count = int(points.shape[0])
    if _LIB is not None:
        return _run_c(points, centers, trig_vec, weights, scales, bias,
                      trig_scale, q_count, int(centers.shape[0]),
                      int(points.shape[1]))
    return _compute_numpy(points, centers, weights, scales, bias, trig_scale,
                          trig_vec, q_count)
