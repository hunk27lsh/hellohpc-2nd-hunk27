import numpy as np

_QBLOCK = 1024
_CBLOCK = 2048

def compute_field(points, centers, weights, scales, bias, trig_scale, trig_vec):
    Q = points.shape[0]
    D = points.shape[1]
    C = centers.shape[0]

    out = np.empty(Q , dtype=np.float32)

    x2 = np.einsum('qd,qd->q', points, points)   # ‖x‖²，(Q,)
    mu2 = np.einsum('cd,cd->c', centers, centers) # ‖μ‖²，(C,)

    QB = _QBLOCK
    CB = _CBLOCK

    for q0 in range(0, Q, QB):
        q1 = min(q0 + QB, Q)
        x = points[q0:q1]                 # (qb, D)
        x2b = x2[q0:q1]                   # (qb,)
        acc = np.zeros(q1 - q0, dtype=np.float32)

        for c0 in range(0, C, CB):
            c1 = min(c0 + CB, C)

            mu = centers[c0:c1]           # (cb, D)
            v = trig_vec[c0:c1]           # (cb, D)
            w = weights[c0:c1]            # (cb,)
            s = scales[c0:c1]             # (cb,)
            b = bias[c0:c1]               # (cb,)
            t = trig_scale[c0:c1]         # (cb,)
            m2 = mu2[c0:c1]               # (cb,)

            # --- distance squared: ||x||^2 + ||mu||^2 - 2 x·mu ---------
            r2 = x @ mu.T                 # (qb, cb)
            r2 *= np.float32(-2.0)
            r2 += x2b[:, None]            # broadcast over cb
            r2 += m2[None, :]             # broadcast over qb

            # --- Gaussian term: w * exp(-s * r2) ------------------------
            r2 *= -s[None, :]             # r2 <- -s * r2
            np.exp(r2, out=r2)
            r2 *= w[None, :]

            # --- Direction term: b * sin(t * x·v) -----------------------
            p = x @ v.T                   # (qb, cb)
            p *= t[None, :]
            np.sin(p, out=p)
            p *= b[None, :]

            # --- Online reduction over C --------------------------------
            acc += np.sum(r2, axis=1)
            acc += np.sum(p, axis=1)

        out[q0:q1] = acc

    return out

