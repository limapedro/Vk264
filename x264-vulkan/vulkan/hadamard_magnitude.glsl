int hadamardMagnitude(inout int block[64], int side) {
    // Use separate signed lanes instead of the packed two-lane C/SIMD representation.
    for (int gap = 1; gap < side; gap *= 2)
        for (int index = 0; index < side*side; ++index)
            if ((index & gap) == 0) {
                int a = block[index], b = block[index+gap];
                block[index] = a + b;
                block[index+gap] = a - b;
            }
    for (int gap = side; gap < side*side; gap *= 2)
        for (int index = 0; index < side*side; ++index)
            if ((index & gap) == 0) {
                int a = block[index], b = block[index+gap];
                block[index] = a + b;
                block[index+gap] = a - b;
            }
    int magnitude = 0;
    for (int index = 0; index < side*side; ++index) magnitude += abs(block[index]);
    return magnitude;
}