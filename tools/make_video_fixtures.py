#!/usr/bin/env python3
"""Create deterministic raw YUV420 fixtures from the videos in D:\\Videos.

The fixtures are small 64x64 / 60-frame crops so they can be encoded by the
parity harness at all 10 presets x 2 entropies without long runs. The source
list and crop parameters are recorded in build/parity/video_manifest.json.
"""
import json, pathlib, subprocess

SRC = pathlib.Path('D:/Videos')
OUT = pathlib.Path(__file__).resolve().parents[1] / 'build/parity/videos'
FFMPEG = pathlib.Path('C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.EXE')
FFPROBE = pathlib.Path('C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffprobe.EXE')
W, H, FRAMES = 64, 64, 60


def probe(path):
    r = subprocess.run([str(FFPROBE), '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height,duration',
                        '-of', 'json', str(path)], capture_output=True, text=True)
    if r.returncode:
        return None
    streams = json.loads(r.stdout).get('streams', [])
    return streams[0] if streams else None


def main():
    if not FFMPEG.exists() or not FFPROBE.exists():
        print('SKIP: ffmpeg/ffprobe not found'); return 2
    OUT.mkdir(parents=True, exist_ok=True)
    candidates = [f for f in SRC.glob('*.mp4')
                  if 200_000 <= f.stat().st_size <= 12_000_000]
    candidates.sort(key=lambda f: f.stat().st_size)
    manifest = []
    for src in candidates[:4]:
        info = probe(src)
        if not info or info['width'] < W or info['height'] < H:
            continue
        name = src.stem[:40].replace(' ', '_')
        yuv = OUT / f'{name}_{W}x{H}_{FRAMES}f.yuv'
        crop_x = max(0, (info['width'] - W) // 2)
        crop_y = max(0, (info['height'] - H) // 2)
        cmd = [str(FFMPEG), '-v', 'error', '-y', '-i', str(src), '-vf',
               f'crop={W}:{H}:{crop_x}:{crop_y}', '-frames:v', str(FRAMES),
               '-pix_fmt', 'yuv420p', '-f', 'rawvideo', str(yuv)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if r.returncode or not yuv.exists():
            print('SKIP', src.name, r.stderr[-200:]); continue
        manifest.append({'name': name, 'source': str(src), 'yuv': str(yuv),
                         'width': W, 'height': H, 'frames': FRAMES,
                         'crop': [crop_x, crop_y]})
        print('created', yuv.name, yuv.stat().st_size, 'bytes')
    (OUT.parent / 'video_manifest.json').write_text(
        json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'{len(manifest)} video fixtures; manifest: {OUT.parent / "video_manifest.json"}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
