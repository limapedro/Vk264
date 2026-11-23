# Next: fix MB5 without disturbing MB0-4

The whole-frame harness shows MB0-4 are fully byte-exact and MB5 is the
first divergence (oracle I4, x264 I8 modes 2,5,2,4). The class-4-only
x3+shortcut patch made MB5 match but also changed MB4 idx3 (x264 mode 2,
oracle 1) because x264's class-4 shortcut applies to within-MB blocks of
edge MBs too.

Next: in `vk_i8x8_decide`, for class-4 blocks use x264's exact
`predict_8x8_mode_available` candidate table (which differs by
`i_neighbour8[idx]`) rather than a single global shortcut, and pass the
true `lt` per block. Validate with `build/wholeframe_diff2.py`: the goal
is a table where MB0-5 all match before moving to MB6.
