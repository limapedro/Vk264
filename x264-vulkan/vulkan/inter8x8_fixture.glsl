struct Luma8Fixture {
    int qp, field, decimate, denoise, cbp;
    uint noiseCount;
    uint multiplier[64], bias[64], noiseOffset[64], residualSum[64];
    int dequant[64];
    uint source[256], pixels[17*stride];
    int coefficients[4][64];
    uint nonzero[120];
};
