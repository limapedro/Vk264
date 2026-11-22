# Full-resolution 1080p60 6000kbps encode report

x264: full 1920x1080, 60 frames, ABR 6000 kbps, preset medium.
oracle: 1920x1088 (height must be divisible by 16), CQP 28 (no ABR support), 2 frames (crashes at 3+).

Byte-exact comparison at these settings is not possible: different rate-control mode,
different dimensions, and the oracle crashes beyond 2 frames at 1080p.

## Results

- warzone2020_verdansk x264: OK codec=h264 profile=High 1920x1080 frames=60 kbps=5558895 duration=0.966687
- warzone2020_verdansk oracle: FAILED rc=3221225477 
- forza x264: OK codec=h264 profile=High 1920x1080 frames=60 kbps=6316120 duration=0.966687
- forza oracle: FAILED rc=3221225477 
- fortnite x264: OK codec=h264 profile=High 1920x1080 frames=60 kbps=4973729 duration=0.966687
- fortnite oracle: FAILED rc=3221225477 
