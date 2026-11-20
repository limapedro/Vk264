struct Luma4Fixture {
    int qp, mode, plane, field, decimate, denoise, cbp;
    uint multiplier[16], bias[16], noiseOffset[16], residualSum[16];
    int dequant[16];
    uint source[256], pixels[17*stride];
    // Scratch copy of the inter prediction so the intra-vs-inter decision can
    // run predictIntra() (which overwrites pixels) and restore it if inter wins.
    uint interPixels[256];
    int intraInP; // 1 when this P-slice macroblock is coded as intra
    int ac[16][16], dcCoefficients[16];
    uint nonzero[120];
};
