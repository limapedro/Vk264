struct ChromaFixture {
    int qp, mode, chroma422, field, denoise, lossless, decimate, lambda2;
    int cbp, dcNonzero, acMask;
    uint dcMultiplier, dcBias;
    int dcDequant;
    uint multiplier[16], bias[16], noiseOffset[16], residualSum[16];
    int dequant[16];
    uint source[128], pixels[17*stride];
    int ac[8][16], dcCoefficients[8];
};
