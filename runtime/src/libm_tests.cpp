// native/libm/nebula_libm.c against the C runtime: the game's own sin, cos, tan, atan, atan2, asin, acos and pow
// within a few ulps of the C runtime's over the arguments the game uses, and their special cases.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
double nebula_sin(double);
double nebula_cos(double);
double nebula_tan(double);
double nebula_atan(double);
double nebula_atan2(double, double);
double nebula_asin(double);
double nebula_acos(double);
double nebula_pow(double, double);
float nebula_sinf(float);
float nebula_atan2f(float, float);
float nebula_powf(float, float);
}

namespace {

int failures = 0;

// Distance in units in the last place of double between two finite results.
double ulps(double a, double b) {
    if (a == b) return 0.0;
    if (std::isnan(a) || std::isnan(b)) return std::isnan(a) && std::isnan(b) ? 0.0 : 1e30;
    const double scale = std::ldexp(1.0, std::ilogb(std::fabs(b) > 0.0 ? b : a) - 52);
    return std::fabs(a - b) / scale;
}

// A tiny deterministic generator (xorshift64*).
std::uint64_t state = 0x9E3779B97F4A7C15ull;
double uniform(double lo, double hi) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const std::uint64_t r = state * 2685821657736338717ull;
    return lo + (hi - lo) * (static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0));
}

template <typename F, typename G>
void sweep1(const char* name, F mine, G reference, double lo, double hi, double limit) {
    double worst = 0.0;
    double at = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double x = uniform(lo, hi);
        const double e = ulps(mine(x), reference(x));
        if (e > worst) {
            worst = e;
            at = x;
        }
    }
    const bool ok = worst <= limit;
    std::printf("%s %s on [%g, %g]: worst %.2f ulp at %.17g\n", ok ? "PASS:" : "FAIL:", name, lo, hi, worst, at);
    if (!ok) ++failures;
}

void check(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS:" : "FAIL:", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    sweep1("sin", nebula_sin, [](double x) { return std::sin(x); }, -10.0, 10.0, 2.0);
    sweep1("sin", nebula_sin, [](double x) { return std::sin(x); }, -1e5, 1e5, 2.0);
    sweep1("cos", nebula_cos, [](double x) { return std::cos(x); }, -10.0, 10.0, 2.0);
    sweep1("cos", nebula_cos, [](double x) { return std::cos(x); }, -1e5, 1e5, 2.0);
    sweep1("tan", nebula_tan, [](double x) { return std::tan(x); }, -1.5, 1.5, 4.0);
    sweep1("tan", nebula_tan, [](double x) { return std::tan(x); }, -1e4, 1e4, 4.0);
    sweep1("atan", nebula_atan, [](double x) { return std::atan(x); }, -10.0, 10.0, 2.0);
    sweep1("atan", nebula_atan, [](double x) { return std::atan(x); }, -1e6, 1e6, 2.0);
    sweep1("asin", nebula_asin, [](double x) { return std::asin(x); }, -1.0, 1.0, 4.0);
    sweep1("acos", nebula_acos, [](double x) { return std::acos(x); }, -1.0, 1.0, 4.0);
    {
        double worst = 0.0;
        for (int i = 0; i < 200000; ++i) {
            const double y = uniform(-1000.0, 1000.0);
            const double x = uniform(-1000.0, 1000.0);
            const double e = ulps(nebula_atan2(y, x), std::atan2(y, x));
            if (e > worst) worst = e;
        }
        std::printf("%s atan2: worst %.2f ulp\n", worst <= 3.0 ? "PASS:" : "FAIL:", worst);
        if (worst > 3.0) ++failures;
    }
    {
        double worst = 0.0;
        double wx = 0.0;
        double wy = 0.0;
        for (int i = 0; i < 200000; ++i) {
            const double x = uniform(0.001, 100.0);
            const double y = uniform(-20.0, 20.0);
            const double e = ulps(nebula_pow(x, y), std::pow(x, y));
            if (e > worst) {
                worst = e;
                wx = x;
                wy = y;
            }
        }
        std::printf("%s pow: worst %.2f ulp at %.17g ^ %.17g\n", worst <= 8.0 ? "PASS:" : "FAIL:", worst, wx, wy);
        if (worst > 8.0) ++failures;
    }
    // Floats: within one float ulp of the C runtime's.
    {
        int off = 0;
        for (int i = 0; i < 200000; ++i) {
            const float x = static_cast<float>(uniform(-100.0, 100.0));
            const float a = nebula_sinf(x);
            const float b = std::sin(x);
            if (std::fabs(a - b) > std::fabs(b) * 1.2e-7f + 1e-38f) ++off;
            const float y = static_cast<float>(uniform(-100.0, 100.0));
            const float c = nebula_atan2f(y, x);
            const float d = std::atan2(y, x);
            if (std::fabs(c - d) > std::fabs(d) * 1.2e-7f + 1e-38f) ++off;
        }
        check("sinf and atan2f within a float ulp", off == 0);
    }
    check("pow special cases", nebula_pow(2.0, 10.0) == 1024.0 && nebula_pow(-2.0, 3.0) == -8.0 &&
                                   nebula_pow(4.0, 0.5) == 2.0 && std::isnan(nebula_pow(-2.0, 0.5)) &&
                                   nebula_pow(0.0, 3.0) == 0.0 && nebula_pow(5.0, 0.0) == 1.0 &&
                                   nebula_powf(2.0f, -1.0f) == 0.5f);
    check("atan2 quadrants", nebula_atan2(0.0, -1.0) == std::atan2(0.0, -1.0) &&
                                 nebula_atan2(-0.0, -1.0) == std::atan2(-0.0, -1.0) &&
                                 nebula_atan2(1.0, 0.0) == std::atan2(1.0, 0.0) &&
                                 nebula_atan2(-1.0, -1.0) == std::atan2(-1.0, -1.0));
    check("sin and cos of special values", nebula_sin(0.0) == 0.0 && nebula_cos(0.0) == 1.0 &&
                                               std::isnan(nebula_sin(std::numeric_limits<double>::infinity())));
    if (failures != 0) {
        std::printf("libm tests: %d failed\n", failures);
        return 1;
    }
    std::printf("libm tests passed\n");
    return 0;
}
