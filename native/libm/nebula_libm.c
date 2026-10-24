/* The game's transcendental functions, the same on every PC (netplay needs every peer to compute the same game).
   The C runtime's (Windows' UCRT) pick code paths by processor (FMA3 or not) and change across Windows updates, so
   the game module carries its own: fdlibm's algorithms (Sun Microsystems' freely distributable math library, the
   base of FreeBSD's and musl's), in plain double arithmetic that the module compiles without fused multiply-adds
   (CMakeLists.txt: -mno-fma -ffp-contract=off). The float functions compute in double and round once.

   Accuracy: within about 1 ulp of the correctly rounded double result for arguments the game uses (angles below
   2^19 * pi/2; larger ones reduce less exactly but still deterministically); runtime/src/libm_tests.cpp compares
   with the C runtime. Exact operations (sqrt, floor, ceil, fmod, ldexp, lround) stay the C runtime's: IEEE 754
   defines their results. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(NEBULA_GAME_MODULE) && (defined(__FMA__) || defined(__AVX__))
#error "nebula_libm must be compiled without FMA or AVX (one result on every x86 processor)"
#endif

#ifdef NEBULA_LIBM_PREFIX
/* Tests compile these under other names beside the C runtime's. */
#define NM(name) nebula_##name
#else
#define NM(name) name
#endif

static uint64_t bits_of(double x) {
    uint64_t u;
    memcpy(&u, &x, sizeof(u));
    return u;
}

/* ---- sin and cos: |x| <= pi/4 kernels, x + y with y the tail ---- */

static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                    S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                    S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;

static double kernel_sin(double x, double y, int iy) {
    const double z = x * x;
    const double v = z * x;
    const double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) {
        return x + v * (S1 + z * r);
    }
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

static double kernel_cos(double x, double y) {
    const double z = x * x;
    const double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    const double hz = 0.5 * z;
    const double w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* x reduced by multiples of pi/2: x = n * pi/2 + (y[0] + y[1]); returns n. Cody-Waite with pi/2 in three parts. */
static const double invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                    pio2_1t = 6.07710050650619224932e-11, pio2_2 = 6.07710050630396597660e-11,
                    pio2_2t = 2.02226624879595063154e-21, pio2_3 = 2.02226624871116645580e-21,
                    pio2_3t = 8.47842766036889956997e-32;

static int rem_pio2(double x, double* y) {
    const double fn = floor(x * invpio2 + 0.5);
    const int n = (int)(int64_t)fn;
    double r = x - fn * pio2_1;
    double w = fn * pio2_1t;
    y[0] = r - w;
    /* A second and a third step when cancellation took most of the bits. */
    const int expx = (int)((bits_of(x) >> 52) & 0x7ff);
    if (expx - (int)((bits_of(y[0]) >> 52) & 0x7ff) > 16) {
        double t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        if (expx - (int)((bits_of(y[0]) >> 52) & 0x7ff) > 49) {
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;
    return n;
}

double NM(sin)(double x) {
    if (!(x - x == 0.0)) {
        return x - x; /* NaN for infinities and NaN */
    }
    if (fabs(x) <= 7.85398163397448278999e-01) {
        return fabs(x) < 7.4505806e-9 ? x : kernel_sin(x, 0.0, 0);
    }
    double y[2];
    const int n = rem_pio2(x, y);
    switch (n & 3) {
    case 0: return kernel_sin(y[0], y[1], 1);
    case 1: return kernel_cos(y[0], y[1]);
    case 2: return -kernel_sin(y[0], y[1], 1);
    default: return -kernel_cos(y[0], y[1]);
    }
}

double NM(cos)(double x) {
    if (!(x - x == 0.0)) {
        return x - x;
    }
    if (fabs(x) <= 7.85398163397448278999e-01) {
        return fabs(x) < 7.4505806e-9 ? 1.0 : kernel_cos(x, 0.0);
    }
    double y[2];
    const int n = rem_pio2(x, y);
    switch (n & 3) {
    case 0: return kernel_cos(y[0], y[1]);
    case 1: return -kernel_sin(y[0], y[1], 1);
    case 2: return -kernel_cos(y[0], y[1]);
    default: return kernel_sin(y[0], y[1], 1);
    }
}

double NM(tan)(double x) {
    if (!(x - x == 0.0)) {
        return x - x;
    }
    double y[2] = {x, 0.0};
    int n = 0;
    if (fabs(x) > 7.85398163397448278999e-01) {
        n = rem_pio2(x, y);
    }
    const double s = kernel_sin(y[0], y[1], 1);
    const double c = kernel_cos(y[0], y[1]);
    return (n & 1) ? -c / s : s / c;
}

/* ---- atan, atan2, asin, acos ---- */

static const double atanhi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                9.82793723247329054082e-01, 1.57079632679489655800e+00};
static const double atanlo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                1.39033110312309984516e-17, 6.12323399573676603587e-17};
static const double aT[] = {3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                            -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
                            6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                            -3.65315727442169155270e-02, 1.62858201153657823623e-02};

double NM(atan)(double x) {
    if (x != x) {
        return x + x;
    }
    const int negative = x < 0.0;
    double ax = fabs(x);
    int id;
    if (ax >= 6.6e19) {
        return negative ? -atanhi[3] - atanlo[3] : atanhi[3] + atanlo[3];
    }
    if (ax < 0.4375) {
        if (ax < 1.0e-29) {
            return x;
        }
        id = -1;
    } else if (ax < 1.1875) {
        if (ax < 0.6875) {
            id = 0;
            ax = (2.0 * ax - 1.0) / (2.0 + ax);
        } else {
            id = 1;
            ax = (ax - 1.0) / (ax + 1.0);
        }
    } else if (ax < 2.4375) {
        id = 2;
        ax = (ax - 1.5) / (1.0 + 1.5 * ax);
    } else {
        id = 3;
        ax = -1.0 / ax;
    }
    const double z = ax * ax;
    const double w = z * z;
    const double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    const double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) {
        const double r = ax - ax * (s1 + s2);
        return negative ? -r : r;
    }
    const double r = atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
    return negative ? -r : r;
}

static const double pi = 3.1415926535897931160e+00, pi_lo = 1.2246467991473531772e-16,
                    pi_o_2 = 1.5707963267948965580e+00, pi_o_4 = 7.8539816339744827900e-01;

double NM(atan2)(double y, double x) {
    if (x != x || y != y) {
        return x + y;
    }
    if (x == 1.0) {
        return NM(atan)(y);
    }
    const int ysign = (bits_of(y) >> 63) != 0;
    const int xsign = (bits_of(x) >> 63) != 0;
    if (y == 0.0) {
        return xsign ? (ysign ? -pi : pi) : y; /* atan2(+-0, -anything) = +-pi; (+-0, +anything) = +-0 */
    }
    if (x == 0.0) {
        return ysign ? -pi_o_2 : pi_o_2;
    }
    if (isinf(x)) {
        if (isinf(y)) {
            const double r = xsign ? 3.0 * pi_o_4 : pi_o_4;
            return ysign ? -r : r;
        }
        const double r = xsign ? pi : 0.0;
        return ysign ? -r : r;
    }
    if (isinf(y)) {
        return ysign ? -pi_o_2 : pi_o_2;
    }
    const double ratio = fabs(y / x);
    double z;
    if (ratio > 1.0e60) {
        z = pi_o_2 + 0.5 * pi_lo;
    } else if (xsign && ratio < 1.0e-60) {
        z = 0.0;
    } else {
        z = NM(atan)(ratio);
    }
    if (!xsign) {
        return ysign ? -z : z;
    }
    const double r = pi - (z - pi_lo);
    return ysign ? -r : r;
}

double NM(asin)(double x) {
    if (!(fabs(x) <= 1.0)) {
        return (x - x) / (x - x);
    }
    return NM(atan2)(x, sqrt((1.0 - x) * (1.0 + x)));
}

double NM(acos)(double x) {
    if (!(fabs(x) <= 1.0)) {
        return (x - x) / (x - x);
    }
    return NM(atan2)(sqrt((1.0 - x) * (1.0 + x)), x);
}

/* ---- pow: exp(y * log(x)) with the logarithm carried in two parts ---- */

static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
                    Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01, Lg3 = 2.857142874366239149e-01,
                    Lg4 = 2.222219843214978396e-01, Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
                    Lg7 = 1.479819860511658591e-01;

/* log(x) for finite x > 0, as hi + lo. */
static void log_parts(double x, double* hi, double* lo) {
    int k = 0;
    uint64_t u = bits_of(x);
    if ((u >> 52) == 0) { /* subnormal: scale up */
        x *= 18014398509481984.0; /* 2^54 */
        k -= 54;
        u = bits_of(x);
    }
    k += (int)((u >> 52) & 0x7ff) - 1023;
    /* Mantissa in [sqrt(2)/2, sqrt(2)). */
    u = (u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m;
    memcpy(&m, &u, sizeof(m));
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        k += 1;
    }
    const double f = m - 1.0;
    const double s = f / (2.0 + f);
    const double z = s * s;
    const double w = z * z;
    const double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    const double R = t2 + t1;
    const double hfsq = 0.5 * f * f;
    const double dk = (double)k;
    /* log(x) = k * ln2 + f - hfsq + s * (hfsq + R): k * ln2_hi + f summed exactly (two-sum; k * ln2_hi has trailing
       zero bits and f = m - 1 is exact), the small terms into the tail. */
    const double a = dk * ln2_hi;
    const double sum = a + f;
    const double bv = sum - a;
    const double err = (a - (sum - bv)) + (f - bv);
    *hi = sum;
    *lo = err + ((s * (hfsq + R) - hfsq) + dk * ln2_lo);
}

static const double invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
                    P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
                    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;

/* exp(hi + lo) for |hi + lo| < 709. */
static double exp_parts(double hi_in, double lo_in) {
    const double x = hi_in + lo_in;
    const double kf = floor(x * invln2 + 0.5);
    const int k = (int)kf;
    const double hi = hi_in - kf * ln2_hi;
    const double lo = kf * ln2_lo - lo_in;
    const double r = hi - lo;
    const double t = r * r;
    const double c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
    return ldexp(y, k);
}

double NM(pow)(double x, double y) {
    if (y == 0.0) {
        return 1.0;
    }
    if (x == 1.0) {
        return 1.0;
    }
    if (x != x || y != y) {
        return x + y;
    }
    /* Whole exponents of modest size: repeated squaring (exact for the small powers the game mostly uses). */
    if (y == floor(y) && fabs(y) <= 64.0) {
        int n = (int)fabs(y);
        double base = x;
        double result = 1.0;
        while (n != 0) {
            if (n & 1) {
                result *= base;
            }
            base *= base;
            n >>= 1;
        }
        return y < 0.0 ? 1.0 / result : result;
    }
    if (x == 0.0) {
        return y < 0.0 ? HUGE_VAL : 0.0;
    }
    int negate = 0;
    if (x < 0.0) {
        if (y != floor(y)) {
            return (x - x) / (x - x); /* NaN: a negative base and a fractional exponent */
        }
        /* An odd whole exponent keeps the sign. */
        negate = fmod(fabs(y), 2.0) == 1.0;
        x = -x;
    }
    if (isinf(x) || isinf(y)) {
        const double r = (x > 1.0) == (y > 0.0) ? HUGE_VAL : 0.0;
        return negate ? -r : r;
    }
    double lhi;
    double llo;
    log_parts(x, &lhi, &llo);
    /* y * log(x) in two parts: split y and log's high part (12-bit halves) so the products are exact. */
    const double split = 134217729.0; /* 2^27 + 1 */
    double t = lhi * split;
    const double lh = t - (t - lhi);
    const double ll = lhi - lh;
    t = y * split;
    const double yh = t - (t - y);
    const double yl = y - yh;
    const double phi = y * lhi;
    const double plo = (((yh * lh - phi) + yh * ll) + yl * lh) + yl * ll + y * llo;
    const double e = phi + plo;
    if (e > 709.78) {
        return negate ? -HUGE_VAL : HUGE_VAL;
    }
    if (e < -745.2) {
        return negate ? -0.0 : 0.0;
    }
    const double r = exp_parts(phi, plo);
    return negate ? -r : r;
}

/* ---- float forms: in double, rounded once ---- */

float NM(sinf)(float x) { return (float)NM(sin)((double)x); }
float NM(cosf)(float x) { return (float)NM(cos)((double)x); }
float NM(tanf)(float x) { return (float)NM(tan)((double)x); }
float NM(atanf)(float x) { return (float)NM(atan)((double)x); }
float NM(atan2f)(float y, float x) { return (float)NM(atan2)((double)y, (double)x); }
float NM(asinf)(float x) { return (float)NM(asin)((double)x); }
float NM(acosf)(float x) { return (float)NM(acos)((double)x); }
float NM(powf)(float x, float y) { return (float)NM(pow)((double)x, (double)y); }
