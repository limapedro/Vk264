struct Luma4Fixture {
    int qp, mode, plane, field, decimate, denoise, cbp;
    uint multiplier[16], bias[16], noiseOffset[16], residualSum[16];
    int dequant[16];
    uint source[256], pixels[17*stride];
    int ac[16][16], dcCoefficients[16];
    uint nonzero[120];
};
