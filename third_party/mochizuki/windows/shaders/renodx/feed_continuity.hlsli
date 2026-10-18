// Shared by the GPU transition and its CPU regression test. Inputs are positive divisors.
float FeedStep(float previous, float target, float attack, float release, float dt) {
    return previous * exp2(clamp(log2(target / previous), -release * dt, attack * dt));
}
